#include "param.h"
#include "types.h"
#include "memlayout.h"
#include "elf.h"
#include "riscv.h"
#include "defs.h"
#include "spinlock.h"
#include "proc.h"
#include "fs.h"
#include "sleeplock.h"
#include "file.h"
#include "fcntl.h"

#define PTE_D (1L << 7) // Bit 7 of the RISC-V page-table entry marks a write.

/*
 * the kernel's page table.
 */
pagetable_t kernel_pagetable;

extern char etext[]; // kernel.ld sets this to end of kernel code.

extern char trampoline[]; // trampoline.S

// Make a direct-map page table for the kernel.
pagetable_t
kvmmake(void)
{
  pagetable_t kpgtbl;

  kpgtbl = (pagetable_t)kalloc();
  memset(kpgtbl, 0, PGSIZE);

  // uart registers
  kvmmap(kpgtbl, UART0, UART0, PGSIZE, PTE_R | PTE_W);

  // virtio mmio disk interface
  kvmmap(kpgtbl, VIRTIO0, VIRTIO0, PGSIZE, PTE_R | PTE_W);

  // PLIC
  kvmmap(kpgtbl, PLIC, PLIC, 0x4000000, PTE_R | PTE_W);

  // map kernel text executable and read-only.
  kvmmap(kpgtbl, KERNBASE, KERNBASE, (uint64)etext - KERNBASE, PTE_R | PTE_X);

  // map kernel data and the physical RAM we'll make use of.
  kvmmap(kpgtbl, (uint64)etext, (uint64)etext, PHYSTOP - (uint64)etext,
         PTE_R | PTE_W);

  // map the trampoline for trap entry/exit to
  // the highest virtual address in the kernel.
  kvmmap(kpgtbl, TRAMPOLINE, (uint64)trampoline, PGSIZE, PTE_R | PTE_X);

  // allocate and map a kernel stack for each process.
  proc_mapstacks(kpgtbl);

  return kpgtbl;
}

// add a mapping to the kernel page table.
// only used when booting.
// does not flush TLB or enable paging.
void
kvmmap(pagetable_t kpgtbl, uint64 va, uint64 pa, uint64 sz, int perm)
{
  if (mappages(kpgtbl, va, sz, pa, perm) != 0)
    panic("kvmmap");
}

// Initialize the kernel_pagetable, shared by all CPUs.
void
kvminit(void)
{
  kernel_pagetable = kvmmake();
}

// Switch the current CPU's h/w page table register to
// the kernel's page table, and enable paging.
void
kvminithart()
{
  // wait for any previous writes to the page table memory to finish.
  sfence_vma();

  w_satp(MAKE_SATP(kernel_pagetable));

  // flush stale entries from the TLB.
  sfence_vma();
}

// Return the address of the PTE in page table pagetable
// that corresponds to virtual address va.  If alloc!=0,
// create any required page-table pages.
//
// The risc-v Sv39 scheme has three levels of page-table
// pages. A page-table page contains 512 64-bit PTEs.
// A 64-bit virtual address is split into five fields:
//   39..63 -- must be zero.
//   30..38 -- 9 bits of level-2 index.
//   21..29 -- 9 bits of level-1 index.
//   12..20 -- 9 bits of level-0 index.
//    0..11 -- 12 bits of byte offset within the page.
pte_t *
walk(pagetable_t pagetable, uint64 va, int alloc)
{
  if (va >= MAXVA)
    panic("walk");

  for (int level = 2; level > 0; level--) {
    pte_t *pte = &pagetable[PX(level, va)];
    if (*pte & PTE_V) {
      pagetable = (pagetable_t)PTE2PA(*pte);
    } else {
      if (!alloc || (pagetable = (pde_t *)kalloc()) == 0)
        return 0;
      memset(pagetable, 0, PGSIZE);
      *pte = PA2PTE(pagetable) | PTE_V;
    }
  }
  return &pagetable[PX(0, va)];
}

// Look up a virtual address, return the physical address,
// or 0 if not mapped.
// Can only be used to look up user pages.
uint64
walkaddr(pagetable_t pagetable, uint64 va)
{
  pte_t *pte;
  uint64 pa;

  if (va >= MAXVA)
    return 0;

  pte = walk(pagetable, va, 0);
  if (pte == 0)
    return 0;
  if ((*pte & PTE_V) == 0)
    return 0;
  if ((*pte & PTE_U) == 0)
    return 0;
  pa = PTE2PA(*pte);
  return pa;
}

// Create PTEs for virtual addresses starting at va that refer to
// physical addresses starting at pa.
// va and size MUST be page-aligned.
// Returns 0 on success, -1 if walk() couldn't
// allocate a needed page-table page.
int
mappages(pagetable_t pagetable, uint64 va, uint64 size, uint64 pa, int perm)
{
  uint64 a, last;
  pte_t *pte;

  if ((va % PGSIZE) != 0)
    panic("mappages: va not aligned");

  if ((size % PGSIZE) != 0)
    panic("mappages: size not aligned");

  if (size == 0)
    panic("mappages: size");

  a = va;
  last = va + size - PGSIZE;
  for (;;) {
    if ((pte = walk(pagetable, a, 1)) == 0)
      return -1;
    if (*pte & PTE_V)
      panic("mappages: remap");
    *pte = PA2PTE(pa) | perm | PTE_V;
    if (a == last)
      break;
    a += PGSIZE;
    pa += PGSIZE;
  }
  return 0;
}

// create an empty user page table.
// returns 0 if out of memory.
pagetable_t
uvmcreate()
{
  pagetable_t pagetable;
  pagetable = (pagetable_t)kalloc();
  if (pagetable == 0)
    return 0;
  memset(pagetable, 0, PGSIZE);
  return pagetable;
}

// Remove npages of mappings starting from va. va must be
// page-aligned. It's OK if the mappings don't exist.
// Optionally free the physical memory.
void
uvmunmap(pagetable_t pagetable, uint64 va, uint64 npages, int do_free)
{
  uint64 a;
  pte_t *pte;

  if ((va % PGSIZE) != 0)
    panic("uvmunmap: not aligned");

  for (a = va; a < va + npages * PGSIZE; a += PGSIZE) {
    if ((pte = walk(pagetable, a, 0)) == 0) // leaf page table entry allocated?
      continue;
    if ((*pte & PTE_V) == 0) // has physical page been allocated?
      continue;
    if (do_free) {
      uint64 pa = PTE2PA(*pte);
      kfree((void *)pa);
    }
    *pte = 0;
  }
}

// Allocate PTEs and physical memory to grow a process from oldsz to
// newsz, which need not be page aligned.  Returns new size or 0 on error.
uint64
uvmalloc(pagetable_t pagetable, uint64 oldsz, uint64 newsz, int xperm)
{
  char *mem;
  uint64 a;

  if (newsz < oldsz)
    return oldsz;

  oldsz = PGROUNDUP(oldsz);
  for (a = oldsz; a < newsz; a += PGSIZE) {
    mem = kalloc();
    if (mem == 0) {
      uvmdealloc(pagetable, a, oldsz);
      return 0;
    }
    memset(mem, 0, PGSIZE);
    if (mappages(pagetable, a, PGSIZE, (uint64)mem, PTE_R | PTE_U | xperm) !=
        0) {
      kfree(mem);
      uvmdealloc(pagetable, a, oldsz);
      return 0;
    }
  }
  return newsz;
}

// Deallocate user pages to bring the process size from oldsz to
// newsz.  oldsz and newsz need not be page-aligned, nor does newsz
// need to be less than oldsz.  oldsz can be larger than the actual
// process size.  Returns the new process size.
uint64
uvmdealloc(pagetable_t pagetable, uint64 oldsz, uint64 newsz)
{
  if (newsz >= oldsz)
    return oldsz;

  if (PGROUNDUP(newsz) < PGROUNDUP(oldsz)) {
    int npages = (PGROUNDUP(oldsz) - PGROUNDUP(newsz)) / PGSIZE;
    uvmunmap(pagetable, PGROUNDUP(newsz), npages, 1);
  }

  return newsz;
}

// Recursively free page-table pages.
// All leaf mappings must already have been removed.
void
freewalk(pagetable_t pagetable)
{
  // there are 2^9 = 512 PTEs in a page table.
  for (int i = 0; i < 512; i++) {
    pte_t pte = pagetable[i];
    if ((pte & PTE_V) && (pte & (PTE_R | PTE_W | PTE_X)) == 0) {
      // this PTE points to a lower-level page table.
      uint64 child = PTE2PA(pte);
      freewalk((pagetable_t)child);
      pagetable[i] = 0;
    } else if (pte & PTE_V) {
      panic("freewalk: leaf");
    }
  }
  kfree((void *)pagetable);
}

// Free user memory pages,
// then free page-table pages.
void
uvmfree(pagetable_t pagetable, uint64 sz)
{
  if (sz > 0)
    uvmunmap(pagetable, 0, PGROUNDUP(sz) / PGSIZE, 1);
  freewalk(pagetable);
}

// Given a parent process's page table, copy
// its memory into a child's page table.
// Copies both the page table and the
// physical memory.
// returns 0 on success, -1 on failure.
// frees any allocated pages on failure.
int
uvmcopy(pagetable_t old, pagetable_t new, uint64 sz)
{
  pte_t *pte;
  uint64 pa, i;
  uint flags;
  char *mem;

  for (i = 0; i < sz; i += PGSIZE) {
    if ((pte = walk(old, i, 0)) == 0)
      continue; // page table entry hasn't been allocated
    if ((*pte & PTE_V) == 0)
      continue; // physical page hasn't been allocated
    pa = PTE2PA(*pte);
    flags = PTE_FLAGS(*pte);
    if ((mem = kalloc()) == 0)
      goto err;
    memmove(mem, (char *)pa, PGSIZE);
    if (mappages(new, i, PGSIZE, (uint64)mem, flags) != 0) {
      kfree(mem);
      goto err;
    }
  }
  return 0;

err:
  uvmunmap(new, 0, i / PGSIZE, 1);
  return -1;
}

// mark a PTE invalid for user access.
// used by exec for the user stack guard page.
void
uvmclear(pagetable_t pagetable, uint64 va)
{
  pte_t *pte;

  pte = walk(pagetable, va, 0);
  if (pte == 0)
    panic("uvmclear");
  *pte &= ~PTE_U;
}

// Copy from kernel to user.
// Copy len bytes from src to virtual address dstva in a given page table.
// Return 0 on success, -1 on error.
int
copyout(pagetable_t pagetable, uint64 psz, uint64 dstva, char *src, uint64 len)
{
  uint64 n, va0, pa0;
  pte_t *pte;

  while (len > 0) {
    va0 = PGROUNDDOWN(dstva);
    if (va0 >= MAXVA)
      return -1;

    pa0 = walkaddr(pagetable, va0);
    if (pa0 == 0) {
      if ((pa0 = vmfault(pagetable, psz, va0, 0)) == 0) {
        return -1;
      }
    }

    pte = walk(pagetable, va0, 0);
    // forbid copyout over read-only user text pages.
    if ((*pte & PTE_W) == 0)
      return -1;

    n = PGSIZE - (dstva - va0);
    if (n > len)
      n = len;
    // Copy directly into RAM, so the CPU does not update the user page entry.
    memmove((void *)(pa0 + (dstva - va0)), src, n);
    *pte |= PTE_D; // Record that the kernel changed the user's page.

    len -= n;
    src += n;
    dstva = va0 + PGSIZE;
  }
  return 0;
}

// Copy from user to kernel.
// Copy len bytes to dst from virtual address srcva in a given page table.
// Return 0 on success, -1 on error.
int
copyin(pagetable_t pagetable, uint64 psz, char *dst, uint64 srcva, uint64 len)
{
  uint64 n, va0, pa0;

  while (len > 0) {
    va0 = PGROUNDDOWN(srcva);
    pa0 = walkaddr(pagetable, va0);
    if (pa0 == 0) {
      if ((pa0 = vmfault(pagetable, psz, va0, 1)) == 0) {
        return -1;
      }
    }
    n = PGSIZE - (srcva - va0);
    if (n > len)
      n = len;
    memmove(dst, (void *)(pa0 + (srcva - va0)), n);

    len -= n;
    dst += n;
    srcva = va0 + PGSIZE;
  }
  return 0;
}

// Copy a null-terminated string from user to kernel.
// Copy bytes to dst from virtual address srcva in a given page table,
// until a '\0', or max.
// Return 0 on success, -1 on error.
int
copyinstr(pagetable_t pagetable, uint64 psz, char *dst, uint64 srcva,
          uint64 max)
{
  uint64 n, va0, pa0;
  int got_null = 0;

  while (got_null == 0 && max > 0) {
    va0 = PGROUNDDOWN(srcva);
    pa0 = walkaddr(pagetable, va0);
    if (pa0 == 0) {
      if ((pa0 = vmfault(pagetable, psz, va0, 1)) == 0) {
        return -1;
      }
    }
    n = PGSIZE - (srcva - va0);
    if (n > max)
      n = max;

    char *p = (char *)(pa0 + (srcva - va0));
    while (n > 0) {
      if (*p == '\0') {
        *dst = '\0';
        got_null = 1;
        break;
      } else {
        *dst = *p;
      }
      --n;
      --max;
      p++;
      dst++;
    }

    srcva = va0 + PGSIZE;
  }
  if (got_null) {
    return 0;
  } else {
    return -1;
  }
}

// allocate and map user memory if process is referencing a page
// that was lazily allocated in sys_sbrk().
// returns 0 if va is invalid or already mapped, or if
// out of physical memory, and physical address if successful.
uint64
vmfault(pagetable_t pagetable, uint64 psz, uint64 va, int read)
{
  uint64 mem;

  if (va >= psz)
    return 0;
  va = PGROUNDDOWN(va);
  if (ismapped(pagetable, va)) {
    return 0;
  }
  mem = (uint64)kalloc();
  if (mem == 0)
    return 0;
  memset((void *)mem, 0, PGSIZE);
  if (mappages(pagetable, va, PGSIZE, mem, PTE_W | PTE_U | PTE_R) != 0) {
    kfree((void *)mem);
    return 0;
  }
  return mem;
}

int
ismapped(pagetable_t pagetable, uint64 va)
{
  pte_t *pte = walk(pagetable, va, 0);
  if (pte == 0) {
    return 0;
  }
  if (*pte & PTE_V) {
    return 1;
  }
  return 0;
}

int
mmapfault(struct proc *p, uint64 faultva, int write)
{
  // A fault can occur at any byte. Round down to the start of its page.
  uint64 va = PGROUNDDOWN(faultva);

  // Find the mapping record that owns this address.
  struct mmap_region *m = 0;
  for (int i = 0; i < NMMAP; i++) {
    struct mmap_region *candidate = &p->mmap_regions[i];

    // The slot must be in use, and va must fall inside its address range.
    if (candidate->file && va >= candidate->addr &&
        va - candidate->addr < candidate->len) {
      m = candidate;
      break; // Mapping ranges are separate, so the search is done.
    }
  }

  // A store requires PROT_WRITE. An existing page can fault on protections
  // and must keep its page-table entry.
  if (!m || (write && !(m->prot & PROT_WRITE)) || ismapped(p->pagetable, va))
    return 0; // Leave this fault to the other xv6 handlers.

  char *mem = kalloc(); // Allocate one physical page of RAM.
  if (!mem)
    return 0; // Allocation failed.
  memset(mem, 0, PGSIZE); // Bytes outside the file or requested range stay zero.

  struct inode *ip = m->file->ip; // Inode for the backing file.
  uint64 rel = va - m->addr;      // Distance from the start of the mapping.
  uint64 off = m->offset + rel;   // File byte offset, independent of the fd.
  ilock(ip);                     // Protect the inode while checking and reading.
  uint64 len = m->len - rel;      // Bytes left in the mapping from this page.
  if (len > PGSIZE)
    len = PGSIZE;                 // Load at most one page for this fault.
  if (off >= ip->size)
    len = 0;                      // The page starts at or past the end of file.
  else if (len > ip->size - off)
    len = ip->size - off;         // Read only the bytes that exist in the file.

  // The 0 says mem is a kernel buffer. Skip the read for an all-zero page.
  int n = len ? readi(ip, 0, (uint64)mem, off, len) : 0;
  iunlock(ip);                    // The file read is finished.

  // RISC-V requires read permission on writable pages.
  int perm = PTE_U | PTE_R;        // Give user mode read access to the page.
  if (m->prot & PROT_WRITE)
    perm |= PTE_W;                 // Add write permission when allowed.

  // Check the read, then connect virtual address va to the RAM page mem.
  if (n != len || mappages(p->pagetable, va, PGSIZE, (uint64)mem, perm) < 0) {
    kfree(mem);                    // Give back the RAM if setup failed.
    return 0;                      // Report that the fault remains unresolved.
  }
  return 1;                        // User mode can retry the faulting instruction.
}

// Find the next resident page and update the caller's address in place.
static pte_t *
mmap_next_page(pagetable_t pagetable, uint64 *addr, uint64 end)
{
  const uint64 leaf_span = 512 * PGSIZE;  // One leaf page table covers 2 MiB.
  while (*addr < end) {                   // Scan only the requested range.
    pte_t *pte = walk(pagetable, *addr, 0); // Look up the entry without allocating.
    if (pte && (*pte & PTE_V))
      return pte;                        // This page has physical memory.
    if (pte)
      *addr += PGSIZE;                   // Skip an empty entry in an existing table.
    else
      *addr = (*addr / leaf_span + 1) * leaf_span; // Skip the missing leaf table.
  }
  return 0;                              // No more resident pages in the range.
}

static void
mmap_writeback(struct mmap_region *m, uint64 va, uint64 pa)
{
  struct inode *ip = m->file->ip; // Get the backing file's inode.
  uint64 rel = va - m->addr;      // Page position within the mapping.
  uint64 off = m->offset + rel;   // Corresponding byte position in the file.

  begin_op();                   // Start an xv6 filesystem log operation.
  ilock(ip);                    // Protect the file size and the write.
  if (off < ip->size) {           // Write only if the page overlaps the file.
    uint64 n = m->len - rel;      // Bytes left in the requested mapping.
    if (n > PGSIZE)
      n = PGSIZE;                // Save at most one page.
    if (n > ip->size - off)
      n = ip->size - off;        // Stop at the current end of the file.
    writei(ip, 0, pa, off, n);   // Write from kernel-accessible RAM.
  }
  iunlock(ip);                  // Release the inode lock.
  end_op();                     // Finish the filesystem log operation.
}

static void
mmap_release(pagetable_t pagetable, struct mmap_region *m, uint64 start, uint64 end)
{
  pte_t *pte; // A page-table entry stores the RAM address and flags.

  // Find each page that was loaded. Resume the search at the next page.
  for (uint64 va = start; (pte = mmap_next_page(pagetable, &va, end)); va += PGSIZE) {
    // A clean page may hold an old copy of bytes another writer updated.
    if (m->flags == MAP_SHARED && (*pte & PTE_D)) // Save only dirty shared pages.
      mmap_writeback(m, va, PTE2PA(*pte));        // Extract the page's RAM address.
    uvmunmap(pagetable, va, 1, 1);                // Remove one page and free its RAM.
  }
}

void
mmap_clear(struct proc *p)
{
  for (int i = 0; i < NMMAP; i++) {
    struct mmap_region *m = &p->mmap_regions[i]; // Get one mapping slot.
    if (!m->file)
      continue;                              // Skip unused slots.

    // Flush and free the pages using the page table that actually owns them.
    mmap_release(p->mmap_pagetable, m, m->addr, m->addr + m->len);
    fileclose(m->file);                       // Drop the mapping's file reference.
    memset(m, 0, sizeof(*m));                 // Clear the slot for reuse.
  }
  p->mmap_pagetable = 0;                      // No page table now owns mappings.
}

int
mmap_fork(struct proc *parent, struct proc *child)
{
  for (int i = 0; i < NMMAP; i++) {
    struct mmap_region *m = &parent->mmap_regions[i]; // Get the parent's slot.
    if (!m->file)
      continue;                                    // Skip unused slots.

    child->mmap_regions[i] = *m;                    // Copy address, length, and flags.
    child->mmap_regions[i].file = filedup(m->file);  // The child owns a reference.
    child->mmap_pagetable = child->pagetable;        // Record which page table owns it.
    pte_t *pte;                                    // The current loaded parent page.

    // Copy the bytes already in RAM. Untouched pages stay lazy in the child.
    for (uint64 va = m->addr;
         (pte = mmap_next_page(parent->pagetable, &va, m->addr + m->len));
         va += PGSIZE) {
      char *mem = kalloc();                       // A separate RAM page for the child.
      if (!mem)
        goto fail;                               // Undo the mapping work so far.
      memmove(mem, (void *)PTE2PA(*pte), PGSIZE);  // Copy what the parent sees.

      // Keep the permissions and clear the child's dirty bit. Only writes
      // made by the child should get written back by the child.
      if (mappages(child->pagetable, va, PGSIZE, (uint64)mem,
                   PTE_FLAGS(*pte) & ~PTE_D) < 0) {
        kfree(mem);                              // The page was never installed.
        goto fail;                               // Clean up the earlier copies too.
      }
    }
  }
  return 0;                                      // All active mappings copied.

fail:
  // Child pages are clean, and the parent still holds each file reference.
  mmap_clear(child);                             // Free the partial copy.
  return -1;                                     // Let kfork fail and free the child.
}

uint64
mmap_limit(struct proc *p)
{
  uint64 limit = TRAPFRAME;                   // Ceiling when no file is mapped.
  for (int i = 0; i < NMMAP; i++)              // Check every mapping slot.
    if (p->mmap_regions[i].file && p->mmap_regions[i].addr < limit)
      limit = p->mmap_regions[i].addr;        // Use it as the new heap ceiling.
  return limit;                              // The heap can grow up to this address.
}

int
munmap_region(struct proc *p, uint64 addr, uint64 len)
{
  // Require a page-aligned start, positive length, and a range below TRAPFRAME.
  if (addr >= TRAPFRAME || addr % PGSIZE || !len || len > TRAPFRAME - addr)
    return -1;

  uint64 end = addr + PGROUNDUP(len); // First address after the pages being removed.
  for (int i = 0; i < NMMAP; i++) {
    struct mmap_region *m = &p->mmap_regions[i];   // Check one mapping slot.
    uint64 mend = m->addr + PGROUNDUP(m->len);    // End of its page-aligned range.

    // The range must be inside one active mapping. Reject a hole in the middle.
    if (!m->file || addr < m->addr || end > mend ||
        (addr != m->addr && end != mend))
      continue;

    // Writeback needs the old address and offset, so release before editing m.
    mmap_release(p->pagetable, m, addr, end);
    if (addr != m->addr) {
      m->len = addr - m->addr;     // Removed the end. Keep the preceding bytes.
    } else if (end != mend) {
      m->addr = end;              // Removed the beginning. Move the start forward.
      m->len -= end - addr;       // Shorten the remaining mapping.
      m->offset += end - addr;    // Advance the remaining mapping's file offset.
    } else {
      fileclose(m->file);         // Removed the whole range. Drop its file reference.
      memset(m, 0, sizeof(*m));   // Make this slot available to the next mmap.
    }
    return 0;                    // The unmap succeeded.
  }
  return -1;                     // No mapping accepted the range.
}
