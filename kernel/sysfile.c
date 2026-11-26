//
// File-system system calls.
// Mostly argument checking, since we don't trust
// user code, and calls into file.c and fs.c.
//

#include "types.h"
#include "riscv.h"
#include "memlayout.h"
#include "defs.h"
#include "param.h"
#include "stat.h"
#include "spinlock.h"
#include "proc.h"
#include "fs.h"
#include "sleeplock.h"
#include "file.h"
#include "fcntl.h"

// Fetch the nth word-sized system call argument as a file descriptor
// and return both the descriptor and the corresponding struct file.
static int
argfd(int n, int *pfd, struct file **pf)
{
  int fd;
  struct file *f;

  argint(n, &fd);
  if (fd < 0 || fd >= NOFILE || (f = myproc()->ofile[fd]) == 0)
    return -1;
  if (pfd)
    *pfd = fd;
  if (pf)
    *pf = f;
  return 0;
}

// Allocate a file descriptor for the given file.
// Takes over file reference from caller on success.
static int
fdalloc(struct file *f)
{
  int fd;
  struct proc *p = myproc();

  for (fd = 0; fd < NOFILE; fd++) {
    if (p->ofile[fd] == 0) {
      p->ofile[fd] = f;
      return fd;
    }
  }
  return -1;
}

uint64
sys_dup(void)
{
  struct file *f;
  int fd;

  if (argfd(0, 0, &f) < 0)
    return -1;
  if ((fd = fdalloc(f)) < 0)
    return -1;
  filedup(f);
  return fd;
}

uint64
sys_read(void)
{
  struct file *f;
  int n;
  uint64 p;

  argaddr(1, &p);
  argint(2, &n);
  if (argfd(0, 0, &f) < 0)
    return -1;
  return fileread(f, p, n);
}

uint64
sys_write(void)
{
  struct file *f;
  int n;
  uint64 p;

  argaddr(1, &p);
  argint(2, &n);
  if (argfd(0, 0, &f) < 0)
    return -1;

  return filewrite(f, p, n);
}

uint64
sys_close(void)
{
  int fd;
  struct file *f;

  if (argfd(0, &fd, &f) < 0)
    return -1;
  myproc()->ofile[fd] = 0;
  fileclose(f);
  return 0;
}

uint64
sys_fstat(void)
{
  struct file *f;
  uint64 st; // user pointer to struct stat

  argaddr(1, &st);
  if (argfd(0, 0, &f) < 0)
    return -1;
  return filestat(f, st);
}

// Create the path new as a link to the same inode as old.
uint64
sys_link(void)
{
  char name[DIRSIZ], new[MAXPATH], old[MAXPATH];
  struct inode *dp, *ip;

  if (argstr(0, old, MAXPATH) < 0 || argstr(1, new, MAXPATH) < 0)
    return -1;

  begin_op();
  if ((ip = namei(old)) == 0) {
    end_op();
    return -1;
  }

  ilock(ip);
  if (ip->type == T_DIR) {
    iunlockput(ip);
    end_op();
    return -1;
  }

  ip->nlink++;
  iupdate(ip);
  iunlock(ip);

  if ((dp = nameiparent(new, name)) == 0)
    goto bad;
  ilock(dp);
  if (dp->dev != ip->dev || dirlink(dp, name, ip->inum) < 0) {
    iunlockput(dp);
    goto bad;
  }
  iunlockput(dp);
  iput(ip);

  end_op();

  return 0;

bad:
  ilock(ip);
  ip->nlink--;
  iupdate(ip);
  iunlockput(ip);
  end_op();
  return -1;
}

// Is the directory dp empty except for "." and ".." ?
static int
isdirempty(struct inode *dp)
{
  int off;
  struct dirent de;

  for (off = 2 * sizeof(de); off < dp->size; off += sizeof(de)) {
    if (readi(dp, 0, (uint64)&de, off, sizeof(de)) != sizeof(de))
      panic("isdirempty: readi");
    if (de.inum != 0)
      return 0;
  }
  return 1;
}

uint64
sys_unlink(void)
{
  struct inode *ip, *dp;
  struct dirent de;
  char name[DIRSIZ], path[MAXPATH];
  uint off;

  if (argstr(0, path, MAXPATH) < 0)
    return -1;

  begin_op();
  if ((dp = nameiparent(path, name)) == 0) {
    end_op();
    return -1;
  }

  ilock(dp);

  // Cannot unlink "." or "..".
  if (namecmp(name, ".") == 0 || namecmp(name, "..") == 0)
    goto bad;

  if ((ip = dirlookup(dp, name, &off)) == 0)
    goto bad;
  ilock(ip);

  if (ip->nlink < 1)
    panic("unlink: nlink < 1");
  if (ip->type == T_DIR && !isdirempty(ip)) {
    iunlockput(ip);
    goto bad;
  }

  memset(&de, 0, sizeof(de));
  if (writei(dp, 0, (uint64)&de, off, sizeof(de)) != sizeof(de))
    panic("unlink: writei");
  if (ip->type == T_DIR) {
    dp->nlink--;
    iupdate(dp);
  }
  iunlockput(dp);

  ip->nlink--;
  iupdate(ip);
  iunlockput(ip);

  end_op();

  return 0;

bad:
  iunlockput(dp);
  end_op();
  return -1;
}

static struct inode *
create_inode(char *path, short type, short major, short minor, char *target)
{
  struct inode *ip, *dp;
  char name[DIRSIZ];

  if ((dp = nameiparent(path, name)) == 0)
    return 0;

  ilock(dp);

  if (dp->nlink == 0) {
    iunlockput(dp);
    return 0;
  }

  if ((ip = dirlookup(dp, name, 0)) != 0) {
    iunlockput(dp);
    ilock(ip);
    if (type == T_FILE && (ip->type == T_FILE || ip->type == T_DEVICE ||
                           ip->type == T_SYMLINK))
      return ip;
    iunlockput(ip);
    return 0;
  }

  if ((ip = ialloc(dp->dev, type)) == 0) {
    iunlockput(dp);
    return 0;
  }

  ilock(ip);
  ip->major = major;
  ip->minor = minor;
  ip->nlink = 1;
  iupdate(ip);

  if (type == T_DIR) { // Create . and .. entries.
    // No ip->nlink++ for ".": avoid cyclic ref count.
    if (dirlink(ip, ".", ip->inum) < 0 || dirlink(ip, "..", dp->inum) < 0)
      goto fail;
  }

  if (target) {
    // Store the full target before publishing the directory entry. The
    // inode can be reclaimed if this write fails, for example on a full disk.
    int len = strlen(target) + 1;
    if (writei(ip, 0, (uint64)target, 0, len) != len)
      goto fail;
  }

  if (dirlink(dp, name, ip->inum) < 0)
    goto fail;

  if (type == T_DIR) {
    // now that success is guaranteed:
    dp->nlink++; // for ".."
    iupdate(dp);
  }

  iunlockput(dp);

  return ip;

fail:
  // something went wrong. de-allocate ip.
  ip->nlink = 0;
  iupdate(ip);
  iunlockput(ip);
  iunlockput(dp);
  return 0;
}

static struct inode *
create(char *path, short type, short major, short minor)
{
  return create_inode(path, type, major, minor, 0);
}

uint64
sys_symlink(void)
{
  char target[MAXPATH], path[MAXPATH];

  if (argstr(0, target, MAXPATH) <= 0 || argstr(1, path, MAXPATH) <= 0)
    return -1;
  begin_op();
  // Store the path now so open can resolve it even if the target appears later.
  struct inode *ip = create_inode(path, T_SYMLINK, 0, 0, target);
  if (ip)
    iunlockput(ip);
  end_op();
  return ip ? 0 : -1;
}

// Resolve the final path component for open(). Each relative target is
// interpreted from the directory containing its symlink.
static struct inode *
follow_symlink(struct inode *ip, char *path)
{
  char name[DIRSIZ], target[MAXPATH];
  struct inode *dir = nameiparent(path, name);

  for (int depth = 0; ip && ip->type == T_SYMLINK; depth++) {
    uint len = ip->size;
    // Bound the lookup even for cycles and require a complete target string.
    int valid = depth < 10 && len > 1 && len <= sizeof(target) &&
                readi(ip, 0, (uint64)target, 0, len) == len && target[len - 1] == 0;
    // Drop the old link's lock before lookup, which can reach that link again.
    iunlockput(ip);
    struct inode *parent = 0;
    ip = valid && (*target == '/' || dir) ? namei_from(dir, target, &parent) : 0;
    if (dir)
      iput(dir);
    dir = parent;
    if (ip)
      ilock(ip);
  }
  if (dir)
    iput(dir);
  return ip;
}

uint64
sys_open(void)
{
  char path[MAXPATH];
  int fd, omode;
  struct file *f;
  struct inode *ip;
  int n;

  argint(1, &omode);
  if ((n = argstr(0, path, MAXPATH)) < 0)
    return -1;

  begin_op();

  if (omode & O_CREATE) {
    ip = create(path, T_FILE, 0, 0);
    if (ip == 0) {
      end_op();
      return -1;
    }
  } else {
    if ((ip = namei(path)) == 0) {
      end_op();
      return -1;
    }
    ilock(ip);
  }
  // O_NOFOLLOW keeps the link inode available for inspecting its target path.
  if (ip->type == T_SYMLINK && !(omode & O_NOFOLLOW) &&
      (ip = follow_symlink(ip, path)) == 0) {
    end_op();
    return -1;
  }
  if (ip->type == T_DIR && omode != O_RDONLY) {
    iunlockput(ip);
    end_op();
    return -1;
  }

  if (ip->type == T_DEVICE && (ip->major < 0 || ip->major >= NDEV)) {
    iunlockput(ip);
    end_op();
    return -1;
  }

  if ((f = filealloc()) == 0 || (fd = fdalloc(f)) < 0) {
    if (f)
      fileclose(f);
    iunlockput(ip);
    end_op();
    return -1;
  }

  if (ip->type == T_DEVICE) {
    f->type = FD_DEVICE;
    f->major = ip->major;
  } else {
    f->type = FD_INODE;
    f->off = 0;
  }
  f->ip = ip;
  f->readable = !(omode & O_WRONLY);
  f->writable = (omode & O_WRONLY) || (omode & O_RDWR);

  if ((omode & O_TRUNC) && ip->type == T_FILE) {
    itrunc(ip);
  }

  iunlock(ip);
  end_op();

  return fd;
}

uint64
sys_mmap(void)
{
  uint64 requested, len, offset; // Address, byte length, and starting file offset.
  int prot, flags;                // Access permissions and shared/private mode.
  struct file *f;                 // Backing file obtained from the descriptor.
  struct proc *p = myproc();      // The mapping belongs to this process.

  argaddr(0, &requested);         // Read the requested address.
  argaddr(1, &len);               // Read the requested byte count.
  argint(2, &prot);               // Read PROT_READ and PROT_WRITE bits.
  argint(3, &flags);              // Read MAP_SHARED or MAP_PRIVATE.
  argaddr(5, &offset);             // Read the file offset. argfd below reads fd.

  // xv6 chooses the address and starts at file offset zero. Require a valid
  // size, supported flags, and a readable regular file. A writable shared
  // mapping also needs permission to update the file.
  if (requested != 0 || len == 0 || len >= TRAPFRAME || offset != 0 ||
      prot == 0 || (prot & ~(PROT_READ | PROT_WRITE)) != 0 ||
      (flags != MAP_SHARED && flags != MAP_PRIVATE) ||
      argfd(4, 0, &f) < 0 || f->type != FD_INODE ||
      f->ip->type != T_FILE || !f->readable ||
      (flags == MAP_SHARED && (prot & PROT_WRITE) && !f->writable))
    return -1;

  struct mmap_region *slot = 0;  // Find a free entry in the mapping table.
  for (int i = 0; i < NMMAP; i++)
    if (!p->mmap_regions[i].file) {
      slot = &p->mmap_regions[i]; // A slot with no file is unused.
      break;                     // Use the first available slot.
    }
  if (!slot)
    return -1;                   // All mapping slots are occupied.

  uint64 span = PGROUNDUP(len);   // Reserve pages, but keep len for byte limits.
  uint64 top = TRAPFRAME;         // Start the search below the trapframe.
  uint64 floor = PGROUNDUP(p->sz); // Stay above the existing process memory.

  while (top >= floor && top - floor >= span) { // While the candidate can fit.
    uint64 addr = top - span;     // Candidate start directly below top.
    uint64 next = top;            // If this changes, an existing mapping overlaps.
    for (int i = 0; i < NMMAP; i++) {
      struct mmap_region *m = &p->mmap_regions[i];
      if (m->file && m->addr < next && addr < m->addr + PGROUNDUP(m->len))
        next = m->addr;          // Move below the overlapping mapping.
    }
    if (next == top) {            // No overlap, so reserve this range.
      // Keep a reference so the file remains available after close(fd).
      *slot = (struct mmap_region){.addr = addr, .len = len, .prot = prot,
                                  .flags = flags, .file = filedup(f)};
      p->mmap_pagetable = p->pagetable; // Record the owner for later cleanup.
      return addr;               // Pages will be loaded on demand by mmapfault.
    }
    top = next;                  // Try the lower gap.
  }
  return -1;                     // The address space has no suitable gap.
}

uint64
sys_munmap(void)
{
  uint64 addr, len;     // Starting address and byte count from user space.
  argaddr(0, &addr);     // Read the first syscall argument.
  argaddr(1, &len);      // Read the second syscall argument.
  return munmap_region(myproc(), addr, len); // Unmap in the current process.
}

uint64
sys_mkdir(void)
{
  char path[MAXPATH];
  struct inode *ip;

  begin_op();
  if (argstr(0, path, MAXPATH) < 0 || (ip = create(path, T_DIR, 0, 0)) == 0) {
    end_op();
    return -1;
  }
  iunlockput(ip);
  end_op();
  return 0;
}

uint64
sys_mknod(void)
{
  struct inode *ip;
  char path[MAXPATH];
  int major, minor;

  begin_op();
  argint(1, &major);
  argint(2, &minor);
  if ((argstr(0, path, MAXPATH)) < 0 ||
      (ip = create(path, T_DEVICE, major, minor)) == 0) {
    end_op();
    return -1;
  }
  iunlockput(ip);
  end_op();
  return 0;
}

uint64
sys_chdir(void)
{
  char path[MAXPATH];
  struct inode *ip;
  struct proc *p = myproc();

  begin_op();
  if (argstr(0, path, MAXPATH) < 0 || (ip = namei(path)) == 0) {
    end_op();
    return -1;
  }
  ilock(ip);
  if (ip->type != T_DIR) {
    iunlockput(ip);
    end_op();
    return -1;
  }
  iunlock(ip);
  iput(p->cwd);
  end_op();
  p->cwd = ip;
  return 0;
}

uint64
sys_exec(void)
{
  char path[MAXPATH], *argv[MAXARG];
  int i;
  uint64 uargv, uarg;

  argaddr(1, &uargv);
  if (argstr(0, path, MAXPATH) < 0) {
    return -1;
  }
  memset(argv, 0, sizeof(argv));
  for (i = 0;; i++) {
    if (i >= NELEM(argv)) {
      goto bad;
    }
    if (fetchaddr(uargv + sizeof(uint64) * i, (uint64 *)&uarg) < 0) {
      goto bad;
    }
    if (uarg == 0) {
      argv[i] = 0;
      break;
    }
    argv[i] = kalloc();
    if (argv[i] == 0)
      goto bad;
    if (fetchstr(uarg, argv[i], PGSIZE) < 0)
      goto bad;
  }

  int ret = kexec(path, argv);
  // A successful exec replaces the program that owned the handler address.
  if (ret >= 0) {
    struct proc *p = myproc();
    p->alarm_interval = 0;
    p->alarm_ticks = 0;
    p->alarm_handler = 0;
    p->alarm_active = 0;
  }

  for (i = 0; i < NELEM(argv) && argv[i] != 0; i++)
    kfree(argv[i]);

  return ret;

bad:
  for (i = 0; i < NELEM(argv) && argv[i] != 0; i++)
    kfree(argv[i]);
  return -1;
}

uint64
sys_pipe(void)
{
  uint64 fdarray; // user pointer to array of two integers
  struct file *rf, *wf;
  int fd0, fd1;
  struct proc *p = myproc();

  argaddr(0, &fdarray);
  if (pipealloc(&rf, &wf) < 0)
    return -1;
  fd0 = -1;
  if ((fd0 = fdalloc(rf)) < 0 || (fd1 = fdalloc(wf)) < 0) {
    if (fd0 >= 0)
      p->ofile[fd0] = 0;
    fileclose(rf);
    fileclose(wf);
    return -1;
  }
  if (copyout(p->pagetable, p->sz, fdarray, (char *)&fd0, sizeof(fd0)) < 0 ||
      copyout(p->pagetable, p->sz, fdarray + sizeof(fd0), (char *)&fd1,
              sizeof(fd1)) < 0) {
    p->ofile[fd0] = 0;
    p->ofile[fd1] = 0;
    fileclose(rf);
    fileclose(wf);
    return -1;
  }
  return 0;
}
