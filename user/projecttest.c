#include "kernel/types.h"
#include "kernel/fcntl.h"
#include "kernel/fs.h"
#include "kernel/param.h"
#include "kernel/riscv.h"
#include "kernel/memlayout.h"
#include "kernel/stat.h"
#include "user/user.h"

#define FAILED_MAP ((void *)-1)
#define CHECK(expr) do { \
  if (!(expr)) { \
    fprintf(2, "  Assertion failed at line %d (%s)\n", __LINE__, #expr); \
    exit(1); \
  } \
} while (0)

static volatile int alarm_count;
static volatile int alarm_nested;
static volatile int alarm_in_handler;
static uint block_data[3 * BSIZE / sizeof(uint)];
static uint block_read[3 * BSIZE / sizeof(uint)];
static char map_data[PGSIZE];

static void
wait_status(int pid, int expected)
{
  int status;
  CHECK(pid > 0);
  CHECK(wait(&status) == pid);
  CHECK(status == expected);
}

static void
run_user_code(void)
{
  for (int i = 0; i < 500000; i++)
    asm volatile("nop");
}

static void
burn_ticks(int duration)
{
  uint start = uptime();
  do {
    run_user_code();
  } while ((uint)uptime() - start < duration);
}

static void
wait_alarms(int count)
{
  uint start = uptime();
  while (alarm_count < count && (uint)uptime() - start < 40)
    run_user_code();
  CHECK(alarm_count >= count);
}

static void
alarm_handler(void)
{
  alarm_count++;
  asm volatile("li t0, 0; li a0, 0" ::: "t0", "a0");
  sigreturn();
  exit(1);
}

static void
alarm_periodic(void)
{
  CHECK(sigalarm(1, alarm_handler) == 0);
  wait_alarms(3);
  CHECK(sigalarm(0, 0) == 0);
  int count = alarm_count;
  burn_ticks(3);
  CHECK(alarm_count == count);
  CHECK(sigreturn() == -1);
  printf("  Observed %d callbacks. The count stayed at %d after disabling the alarm.\n",
         count, alarm_count);
}

static void
alarm_cpu_time(void)
{
  CHECK(sigalarm(8, alarm_handler) == 0);
  CHECK(pause(20) == 0);
  CHECK(alarm_count == 0);
  CHECK(sigalarm(-1, 0) == -1);
  wait_alarms(1);
  CHECK(sigalarm(0, 0) == 0);
  printf("  No callbacks during 20 ticks of sleep. Observed %d after running user code.\n",
         alarm_count);
}

static void
alarm_registers(void)
{
  uint64 a0, t0;
  CHECK(sigalarm(1, alarm_handler) == 0);
  asm volatile(
    "li a0, 0x13579bdf2468ace0\n"
    "li t0, 0x12345678\n"
    "li t2, 120000000\n"
    "sw zero, 0(%[count])\n"
    "1: lw t1, 0(%[count])\n"
    "bnez t1, 2f\n"
    "addi t2, t2, -1\n"
    "bnez t2, 1b\n"
    "2: mv %[a0], a0\n"
    "mv %[t0], t0\n"
    : [a0] "=&r"(a0), [t0] "=&r"(t0)
    : [count] "r"(&alarm_count)
    : "a0", "t0", "t1", "t2", "memory");
  CHECK(sigalarm(0, 0) == 0);
  CHECK(alarm_count > 0);
  CHECK(a0 == 0x13579bdf2468ace0UL);
  CHECK(t0 == 0x12345678UL);
  printf("  Restored a0=0x%lx and t0=0x%lx after the handler cleared both.\n", a0, t0);
}

static void
long_handler(void)
{
  if (alarm_in_handler) {
    alarm_nested++;
    sigreturn();
    exit(1);
  }
  alarm_in_handler = 1;
  burn_ticks(4);
  CHECK(sigalarm(0, 0) == 0);
  alarm_count++;
  alarm_in_handler = 0;
  sigreturn();
  exit(1);
}

static void
alarm_nonreentrant(void)
{
  CHECK(sigalarm(1, long_handler) == 0);
  wait_alarms(1);
  burn_ticks(3);
  CHECK(alarm_count == 1);
  CHECK(alarm_nested == 0);
  printf("  Observed %d completed callback and %d nested callbacks.\n",
         alarm_count, alarm_nested);
}

static void
alarm_process(void)
{
  CHECK(sigalarm(1, alarm_handler) == 0);
  int pid = fork();
  CHECK(pid >= 0);
  if (pid == 0) {
    int count = alarm_count;
    burn_ticks(5);
    CHECK(alarm_count == count);
    exit(0);
  }
  wait_status(pid, 0);
  wait_alarms(1);
  CHECK(sigalarm(0, 0) == 0);
}

static int
open_file(const char *path, int mode)
{
  int fd = open(path, mode);
  CHECK(fd >= 0);
  return fd;
}

static void
put_byte(const char *path, char value)
{
  int fd = open_file(path, O_CREATE | O_WRONLY | O_TRUNC);
  CHECK(write(fd, &value, 1) == 1);
  CHECK(close(fd) == 0);
}

static void
expect_byte(const char *path, char expected)
{
  int fd = open_file(path, O_RDONLY);
  char actual;
  CHECK(read(fd, &actual, 1) == 1);
  CHECK(actual == expected);
  CHECK(close(fd) == 0);
}

static void
block_pattern(int first, int bytes)
{
  for (uint i = 0; i < bytes / sizeof(uint); i++)
    block_data[i] = 0x12345678U ^ (first * (BSIZE / sizeof(uint)) + i);
}

static void
report_blocks(const char *action, int first, int bytes, int total)
{
  const int interval = 4096;
  int done = first + bytes / BSIZE;
  if (total > interval && (done == total || done / interval != first / interval))
    printf("    %s %d/%d blocks (%d%%)\n", action, done, total, done * 100 / total);
}

static void
write_blocks(int fd, int blocks)
{
  for (int first = 0; first < blocks; first += 3) {
    int bytes = (blocks - first < 3 ? blocks - first : 3) * BSIZE;
    block_pattern(first, bytes);
    CHECK(write(fd, block_data, bytes) == bytes);
    report_blocks("Wrote", first, bytes, blocks);
  }
}

static void
read_blocks(int fd, int blocks)
{
  for (int first = 0; first < blocks; first += 3) {
    int bytes = (blocks - first < 3 ? blocks - first : 3) * BSIZE;
    block_pattern(first, bytes);
    CHECK(read(fd, block_read, bytes) == bytes);
    CHECK(memcmp(block_data, block_read, bytes) == 0);
    report_blocks("Verified", first, bytes, blocks);
  }
  CHECK(read(fd, block_read, 1) == 0);
}

static void
file_boundaries(void)
{
  const struct {
    int blocks;
    const char *last_block;
  } sizes[] = {
    {NDIRECT, "last direct block"},
    {NDIRECT + 1, "first singly indirect block"},
    {NDIRECT + NINDIRECT, "last singly indirect block"},
    {NDIRECT + NINDIRECT + 1, "first doubly indirect block"},
    {NDIRECT + 2 * NINDIRECT + 1, "first block in the next doubly indirect table"},
  };
  for (uint i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
    int blocks = sizes[i].blocks;
    printf("  Write and compare %d blocks through the %s.\n", blocks, sizes[i].last_block);
    int fd = open_file("file", O_CREATE | O_WRONLY | O_TRUNC);
    write_blocks(fd, blocks);
    CHECK(close(fd) == 0);
    fd = open_file("file", O_RDONLY);
    read_blocks(fd, blocks);
    CHECK(close(fd) == 0);
    CHECK(unlink("file") == 0);
  }
}

static void
file_capacity(void)
{
  CHECK(MAXFILE == 65803);
  int fd = open_file("file", O_CREATE | O_WRONLY | O_TRUNC);
  printf("  Write %lu blocks of patterned data to reach the file-size limit.\n", MAXFILE);
  write_blocks(fd, MAXFILE);
  CHECK(write(fd, block_data, 1) == -1);
  struct stat st;
  CHECK(fstat(fd, &st) == 0);
  CHECK(st.size == (uint64)MAXFILE * BSIZE);
  CHECK(close(fd) == 0);
  printf("  Confirmed %lu bytes and rejection of a one-byte append.\n", st.size);

  printf("  Reopen the file and compare every byte with the written pattern.\n");
  fd = open_file("file", O_RDONLY);
  read_blocks(fd, MAXFILE);
  printf("  Fill the remaining disk space, then try creating a symlink.\n"
         "  Both operations should report that the disk is out of blocks.\n");
  int other = open_file("other", O_CREATE | O_WRONLY);
  int spare = 0;
  while (write(other, block_data, BSIZE) == BSIZE)
    spare++;
  CHECK(spare > 0 && spare < 4096);
  printf("    Used %d more data blocks before the disk filled.\n", spare);
  CHECK(symlink("file", "link") == -1);
  CHECK(open("link", O_RDONLY | O_NOFOLLOW) == -1);
  CHECK(close(other) == 0);
  CHECK(unlink("other") == 0);

  printf("  Truncate the full-size file and check that its size is zero.\n");
  int trunc = open_file("file", O_WRONLY | O_TRUNC);
  CHECK(fstat(trunc, &st) == 0 && st.size == 0);
  CHECK(read(fd, block_read, 1) == 0);
  CHECK(close(fd) == 0);

  printf("  Write 4096 blocks to a new file to prove that truncation freed space.\n");
  other = open_file("other", O_CREATE | O_WRONLY);
  write_blocks(other, 4096);
  CHECK(close(other) == 0);
  CHECK(unlink("other") == 0);
  printf("  Grow the truncated file into doubly indirect blocks and verify its data.\n");
  write_blocks(trunc, NDIRECT + NINDIRECT + 1);
  CHECK(close(trunc) == 0);
  fd = open_file("file", O_RDONLY);
  read_blocks(fd, NDIRECT + NINDIRECT + 1);
  CHECK(close(fd) == 0);
  CHECK(unlink("file") == 0);
}

static void
symlink_follow(void)
{
  put_byte("file", 'A');
  CHECK(symlink("file", "link") == 0);
  expect_byte("link", 'A');

  int fd = open_file("link", O_RDONLY | O_NOFOLLOW);
  struct stat st;
  CHECK(fstat(fd, &st) == 0 && st.type == T_SYMLINK);
  char target[5];
  CHECK(read(fd, target, sizeof(target)) == sizeof(target));
  CHECK(memcmp(target, "file", sizeof(target)) == 0);
  CHECK(close(fd) == 0);

  fd = open_file("link", O_CREATE | O_WRONLY | O_TRUNC);
  CHECK(write(fd, "B", 1) == 1);
  CHECK(close(fd) == 0);
  expect_byte("file", 'B');
  CHECK(link("link", "hard") == 0);
  CHECK(unlink("link") == 0);
  expect_byte("hard", 'B');
  CHECK(unlink("hard") == 0);
  expect_byte("file", 'B');
}

static void
symlink_dangling(void)
{
  CHECK(symlink("file", "link") == 0);
  CHECK(open("link", O_RDONLY) == -1);
  int fd = open_file("link", O_RDONLY | O_NOFOLLOW);
  CHECK(close(fd) == 0);
  put_byte("file", 'B');
  expect_byte("link", 'B');
  CHECK(unlink("file") == 0);
  CHECK(open("link", O_RDONLY) == -1);
  put_byte("file", 'C');
  expect_byte("link", 'C');
}

static void
symlink_paths(void)
{
  CHECK(mkdir("dir") == 0);
  put_byte("file", 'A');
  put_byte("dir/file", 'B');
  CHECK(symlink("file", "dir/link") == 0);
  expect_byte("dir/link", 'B');
  CHECK(symlink("dir/link", "link") == 0);
  expect_byte("link", 'B');
  CHECK(unlink("dir/link") == 0);
  CHECK(symlink("../file", "dir/link") == 0);
  expect_byte("link", 'A');

  CHECK(symlink("/README", "next") == 0);
  struct stat from_link, from_target;
  CHECK(stat("next", &from_link) == 0);
  CHECK(stat("/README", &from_target) == 0);
  CHECK(from_link.ino == from_target.ino);
  CHECK(unlink("next") == 0);
  CHECK(symlink("dir", "next") == 0);
  CHECK(stat("next", &from_link) == 0 && from_link.type == T_DIR);
}

static void
symlink_depth(void)
{
  CHECK(symlink("link", "link") == 0);
  CHECK(open("link", O_RDONLY) == -1);
  CHECK(unlink("link") == 0);
  CHECK(symlink("next", "link") == 0);
  CHECK(symlink("link", "next") == 0);
  CHECK(open("link", O_RDONLY) == -1);

  put_byte("file", 'D');
  char path[] = "c0";
  char target[] = "c0";
  for (int i = 0; i < 10; i++) {
    path[1] = '0' + i;
    if (i == 0)
      CHECK(symlink("file", path) == 0);
    else {
      target[1] = path[1] - 1;
      CHECK(symlink(target, path) == 0);
    }
  }
  expect_byte("c9", 'D');
  CHECK(symlink("c9", "other") == 0);
  CHECK(open("other", O_RDONLY) == -1);
}

static void
symlink_errors(void)
{
  put_byte("file", 'E');
  CHECK(symlink("file", "dir/link") == -1);
  CHECK(symlink("file", "file") == -1);
  CHECK(symlink("file", "file/link") == -1);
  CHECK(symlink("file", "") == -1);
  CHECK(symlink("", "link") == -1);
  CHECK(symlink("file", "link") == 0);
  CHECK(symlink("absent", "link") == -1);
  char path[MAXPATH + 1];
  memset(path, 'x', sizeof(path) - 1);
  path[MAXPATH] = 0;
  CHECK(symlink(path, "next") == -1);
  CHECK(symlink("file", path) == -1);
  path[MAXPATH - 1] = 0;
  CHECK(symlink(path, "next") == 0);
  int fd = open_file("next", O_RDONLY | O_NOFOLLOW);
  char target[MAXPATH];
  CHECK(read(fd, target, sizeof(target)) == sizeof(target));
  CHECK(memcmp(path, target, sizeof(target)) == 0);
  CHECK(close(fd) == 0);
  expect_byte("link", 'E');
}

static void
symlink_concurrent(void)
{
  put_byte("file", 'F');
  for (int i = 0; i < 2; i++) {
    int pid = fork();
    CHECK(pid >= 0);
    if (pid == 0)
      exit(symlink("file", "link") == 0 ? 0 : 1);
  }
  int first, second;
  CHECK(wait(&first) > 0);
  CHECK(wait(&second) > 0);
  CHECK((first == 0 && second == 1) || (first == 1 && second == 0));
  expect_byte("link", 'F');
}

static int
make_pages(int pages, int last_bytes)
{
  int fd = open_file("file", O_CREATE | O_RDWR | O_TRUNC);
  for (int i = 0; i < pages; i++) {
    int bytes = i == pages - 1 ? last_bytes : PGSIZE;
    memset(map_data, 'A' + i, bytes);
    CHECK(write(fd, map_data, bytes) == bytes);
  }
  CHECK(close(fd) == 0);
  return open_file("file", O_RDWR);
}

static char *
map_file(int fd, uint64 len, int prot, int flags)
{
  char *p = mmap(0, len, prot, flags, fd, 0);
  CHECK(p != FAILED_MAP);
  CHECK((uint64)p % PGSIZE == 0);
  return p;
}

static void
check_pages(const char *expected, int pages, int last_bytes)
{
  struct stat st;
  int fd = open_file("file", O_RDONLY);
  CHECK(fstat(fd, &st) == 0);
  CHECK(st.size == (pages - 1) * PGSIZE + last_bytes);
  for (int i = 0; i < pages; i++) {
    int bytes = i == pages - 1 ? last_bytes : PGSIZE;
    CHECK(read(fd, map_data, bytes) == bytes);
    CHECK(map_data[0] == expected[i]);
    CHECK(map_data[bytes - 1] == 'A' + i);
  }
  CHECK(close(fd) == 0);
}

static void
mmap_lazy(void)
{
  int fd = make_pages(2, 19);
  uint64 len = 256UL * 1024 * 1024;
  char *p = map_file(fd, len, PROT_READ, MAP_PRIVATE);
  CHECK(close(fd) == 0);
  CHECK(p[len - 1] == 0);
  CHECK(p[PGSIZE] == 'B' && p[PGSIZE + 18] == 'B');
  CHECK(p[PGSIZE + 19] == 0 && p[0] == 'A');
  CHECK(munmap(p, len) == 0);
  check_pages("AB", 2, 19);
}

static void
mmap_shared(void)
{
  int fd = make_pages(2, 29);
  char c;
  CHECK(read(fd, &c, 1) == 1 && c == 'A');
  char *p = map_file(fd, 3 * PGSIZE, PROT_READ | PROT_WRITE, MAP_SHARED);
  CHECK(p[PGSIZE] == 'B');
  p[0] = 'X';
  p[PGSIZE] = 'Y';
  p[2 * PGSIZE] = 'Z';
  CHECK(munmap(p, 3 * PGSIZE) == 0);
  CHECK(read(fd, &c, 1) == 1 && c == 'A');
  CHECK(close(fd) == 0);
  check_pages("XY", 2, 29);
}

static void
mmap_private(void)
{
  int fd = make_pages(2, PGSIZE);
  CHECK(close(fd) == 0);
  fd = open_file("file", O_RDONLY);
  char *p = map_file(fd, 2 * PGSIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE);
  p[0] = 'X';
  CHECK(p[0] == 'X' && p[PGSIZE] == 'B');
  CHECK(munmap(p, 2 * PGSIZE) == 0);
  CHECK(close(fd) == 0);
  check_pages("AB", 2, PGSIZE);
}

static void
mmap_partial(void)
{
  int fd = make_pages(4, 7);
  char *p = map_file(fd, 3 * PGSIZE + 7, PROT_READ | PROT_WRITE, MAP_SHARED);
  CHECK(close(fd) == 0);
  p[0] = 'W';
  p[3 * PGSIZE] = 'Z';
  CHECK(munmap(p, PGSIZE) == 0);
  CHECK(munmap(p + 3 * PGSIZE, 7) == 0);
  check_pages("WBCZ", 4, 7);
  CHECK(p[PGSIZE] == 'B' && p[2 * PGSIZE] == 'C');
  p[PGSIZE] = 'X';
  p[2 * PGSIZE] = 'Y';
  CHECK(munmap(p + PGSIZE, 2 * PGSIZE) == 0);
  check_pages("WXYZ", 4, 7);
}

static void
mmap_dirty_pages(void)
{
  int fd = make_pages(2, PGSIZE);
  char *p = map_file(fd, PGSIZE + 3, PROT_READ | PROT_WRITE, MAP_SHARED);
  CHECK(p[0] == 'A');
  put_byte("other", 'X');
  int src = open_file("other", O_RDONLY);
  CHECK(p[PGSIZE] == 'B');
  CHECK(read(src, p + PGSIZE, 1) == 1 && p[PGSIZE] == 'X');
  CHECK(close(src) == 0);

  // Another writer can update a page that this mapping only read.
  CHECK(write(fd, "Y", 1) == 1);
  p[PGSIZE + 3] = 'Z';
  CHECK(munmap(p, PGSIZE + 3) == 0);
  CHECK(close(fd) == 0);

  check_pages("YX", 2, PGSIZE);
  fd = open_file("file", O_RDONLY);
  CHECK(read(fd, map_data, PGSIZE) == PGSIZE);
  CHECK(read(fd, map_data, PGSIZE) == PGSIZE);
  CHECK(map_data[3] == 'B');
  CHECK(close(fd) == 0);
}

static void
mmap_invalid(void)
{
  int fd = make_pages(1, PGSIZE);
  printf("  Try unsupported addresses, lengths, protections, flags, and offsets.\n");
  CHECK(mmap((void *)PGSIZE, PGSIZE, PROT_READ, MAP_PRIVATE, fd, 0) == FAILED_MAP);
  CHECK(mmap(0, 0, PROT_READ, MAP_PRIVATE, fd, 0) == FAILED_MAP);
  CHECK(mmap(0, (uint64)-1, PROT_READ, MAP_PRIVATE, fd, 0) == FAILED_MAP);
  CHECK(mmap(0, PGSIZE, 0, MAP_PRIVATE, fd, 0) == FAILED_MAP);
  CHECK(mmap(0, PGSIZE, 64, MAP_PRIVATE, fd, 0) == FAILED_MAP);
  CHECK(mmap(0, PGSIZE, PROT_READ, MAP_SHARED | MAP_PRIVATE, fd, 0) == FAILED_MAP);
  CHECK(mmap(0, PGSIZE, PROT_READ, MAP_PRIVATE, fd, 1) == FAILED_MAP);
  CHECK(mmap(0, PGSIZE, PROT_READ, MAP_PRIVATE, -1, 0) == FAILED_MAP);

  printf("  Check access modes and reject mappings of a directory or pipe.\n");
  int ro = open_file("file", O_RDONLY);
  int wo = open_file("file", O_WRONLY);
  int dir = open_file(".", O_RDONLY);
  int pipes[2];
  CHECK(pipe(pipes) == 0);
  CHECK(mmap(0, PGSIZE, PROT_WRITE, MAP_SHARED, ro, 0) == FAILED_MAP);
  CHECK(mmap(0, PGSIZE, PROT_READ, MAP_PRIVATE, wo, 0) == FAILED_MAP);
  CHECK(mmap(0, PGSIZE, PROT_READ, MAP_PRIVATE, dir, 0) == FAILED_MAP);
  CHECK(mmap(0, PGSIZE, PROT_READ, MAP_PRIVATE, pipes[0], 0) == FAILED_MAP);

  printf("  Reject invalid unmap ranges and check that all three pages still read.\n");
  char *p = map_file(fd, 3 * PGSIZE, PROT_READ, MAP_PRIVATE);
  CHECK(munmap(p + 1, PGSIZE) == -1);
  CHECK(munmap(p, 0) == -1);
  CHECK(munmap(p, (uint64)-1) == -1);
  CHECK(munmap(p, 4 * PGSIZE) == -1);
  CHECK(munmap(p + PGSIZE, PGSIZE) == -1);
  CHECK(p[0] == 'A' && p[PGSIZE] == 0 && p[2 * PGSIZE] == 0);
  CHECK(munmap(p, 3 * PGSIZE) == 0);
  CHECK(munmap(p, PGSIZE) == -1);
}

static void
mmap_permissions(void)
{
  int fd = make_pages(1, PGSIZE);
  for (int resident = 0; resident < 2; resident++) {
    printf("  Attempt a write to a read-only page %s a read. Expect a child trap.\n",
           resident ? "after" : "before");
    char *p = map_file(fd, PGSIZE, PROT_READ, MAP_SHARED);
    int pid = fork();
    CHECK(pid >= 0);
    if (pid == 0) {
      volatile char *page = p;
      if (resident)
        CHECK(page[0] == 'A');
      page[0] = 'X';
      exit(1);
    }
    wait_status(pid, -1);
    CHECK(munmap(p, PGSIZE) == 0);
  }
  printf("  Confirm that a writable shared mapping saves a change to the file.\n");
  char *p = map_file(fd, PGSIZE, PROT_WRITE, MAP_SHARED);
  p[0] = 'Y';
  CHECK(munmap(p, PGSIZE) == 0);
  CHECK(close(fd) == 0);
  check_pages("Y", 1, PGSIZE);
}

static void
mmap_unmapped_access(void)
{
  int fd = make_pages(1, PGSIZE);
  char *p = map_file(fd, PGSIZE, PROT_READ, MAP_PRIVATE);
  CHECK(p[0] == 'A');
  CHECK(munmap(p, PGSIZE) == 0);
  int pid = fork();
  CHECK(pid >= 0);
  if (pid == 0) {
    volatile char value = *(volatile char *)p;
    exit(value);
  }
  wait_status(pid, -1);
  CHECK(close(fd) == 0);
}

static void
mmap_slots(void)
{
  int fd = make_pages(1, PGSIZE);
  char *pages[NMMAP];
  for (int i = 0; i < NMMAP; i++) {
    pages[i] = map_file(fd, (i + 1) * PGSIZE, PROT_READ, MAP_PRIVATE);
    for (int j = 0; j < i; j++)
      CHECK(pages[i] + (i + 1) * PGSIZE <= pages[j] ||
            pages[j] + (j + 1) * PGSIZE <= pages[i]);
  }
  printf("  Filled %d mapping slots. Verify that freeing one permits a new map.\n", NMMAP);
  CHECK(mmap(0, PGSIZE, PROT_READ, MAP_PRIVATE, fd, 0) == FAILED_MAP);
  CHECK(munmap(pages[1], 2 * PGSIZE) == 0);
  pages[1] = map_file(fd, 2 * PGSIZE, PROT_READ, MAP_PRIVATE);
  CHECK(close(fd) == 0);
  for (int i = 0; i < NMMAP; i++) {
    CHECK(pages[i][0] == 'A');
    CHECK(munmap(pages[i], (i + 1) * PGSIZE) == 0);
  }
}

static void
mmap_fork_private(void)
{
  int fd = make_pages(2, PGSIZE);
  char *p = map_file(fd, 2 * PGSIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE);
  CHECK(close(fd) == 0);
  p[0] = 'X';
  int pid = fork();
  CHECK(pid >= 0);
  if (pid == 0) {
    CHECK(p[0] == 'X');
    CHECK(p[PGSIZE] == 'B');
    p[0] = 'Y';
    CHECK(munmap(p, 2 * PGSIZE) == 0);
    exit(0);
  }
  wait_status(pid, 0);
  CHECK(p[0] == 'X');
  CHECK(munmap(p, 2 * PGSIZE) == 0);
  check_pages("AB", 2, PGSIZE);
}

static void
mmap_fork_exit(void)
{
  int fd = make_pages(3, 13);
  char *p = map_file(fd, 3 * PGSIZE, PROT_READ | PROT_WRITE, MAP_SHARED);
  CHECK(close(fd) == 0);
  p[0] = 'X';

  int pid = fork();
  CHECK(pid >= 0);
  if (pid == 0) {
    CHECK(p[0] == 'X');
    CHECK(p[PGSIZE] == 'B');
    p[PGSIZE] = 'Y';
    exit(0);
  }
  wait_status(pid, 0);
  check_pages("AYC", 3, 13);
  CHECK(p[2 * PGSIZE] == 'C');
  p[2 * PGSIZE] = 'Z';
  CHECK(munmap(p, 3 * PGSIZE) == 0);
  check_pages("XYZ", 3, 13);
}

static void
mmap_private_exit(void)
{
  int fd = make_pages(1, PGSIZE);
  int pid = fork();
  CHECK(pid >= 0);
  if (pid == 0) {
    char *p = map_file(fd, PGSIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE);
    p[0] = 'X';
    CHECK(p[0] == 'X');
    exit(0);
  }
  wait_status(pid, 0);
  CHECK(close(fd) == 0);
  check_pages("A", 1, PGSIZE);
}

static void
mmap_exec(void)
{
  int fd = make_pages(2, PGSIZE);
  int pid = fork();
  CHECK(pid >= 0);
  if (pid == 0) {
    char *p = map_file(fd, 2 * PGSIZE, PROT_READ | PROT_WRITE, MAP_SHARED);
    CHECK(close(fd) == 0);
    p[0] = 'X';
    char *args[] = {"echo", "  /echo ran after successful exec.", 0};
    CHECK(exec("missing", args) == -1);
    CHECK(p[0] == 'X');
    p[PGSIZE] = 'Y';
    exec("/echo", args);
    exit(1);
  }
  wait_status(pid, 0);
  CHECK(close(fd) == 0);
  check_pages("XY", 2, PGSIZE);
}

static void
mmap_heap(void)
{
  int fd = make_pages(1, PGSIZE);
  uint64 limit = PGROUNDUP((uint64)sbrk(0)) + PGSIZE;
  char *p = map_file(fd, TRAPFRAME - limit, PROT_READ, MAP_PRIVATE);
  CHECK((uint64)p == limit);
  CHECK(sbrk(2 * PGSIZE) == SBRK_ERROR);
  CHECK(sbrklazy(2 * PGSIZE) == SBRK_ERROR);
  CHECK(p[0] == 'A');
  CHECK(munmap(p, TRAPFRAME - limit) == 0);
  CHECK(sbrk(2 * PGSIZE) != SBRK_ERROR);
  CHECK(close(fd) == 0);
}

static const struct {
  const char *name;
  void (*run)(void);
  const char *checks;
} cases[] = {
  {"alarm-periodic", alarm_periodic,
   "Wait for three callbacks, disable the alarm, and verify they stop."},
  {"alarm-cpu-time", alarm_cpu_time,
   "Check that sleep consumes no alarm ticks and user CPU time does.\n  Also verify that a negative interval is rejected."},
  {"alarm-registers", alarm_registers,
   "Check that sigreturn restores a0 and t0 after a timer interrupt."},
  {"alarm-nonreentrant", alarm_nonreentrant,
   "Keep a handler busy for several ticks, then disable its alarm."},
  {"alarm-process", alarm_process,
   "Arm the parent's alarm and check that the child receives no callbacks."},
  {"file-boundaries", file_boundaries,
   "Check data across direct, singly indirect, and doubly indirect blocks."},
  {"file-capacity", file_capacity,
   "Check the file-size limit, disk exhaustion, and block reuse."},
  {"symlink-follow", symlink_follow,
   "Follow a link, open it with O_NOFOLLOW, and test writes and unlink."},
  {"symlink-dangling", symlink_dangling,
   "Open a link with its target missing, created, removed, and recreated."},
  {"symlink-paths", symlink_paths,
   "Resolve relative, absolute, and parent paths, including link chains."},
  {"symlink-depth", symlink_depth,
   "Reject cycles. Check that ten links resolve and eleven fail."},
  {"symlink-errors", symlink_errors,
   "Reject invalid or duplicate paths and check the maximum target length."},
  {"symlink-concurrent", symlink_concurrent,
   "Race two children to create one link. Exactly one should succeed."},
  {"mmap-lazy", mmap_lazy,
   "Map 256 MiB, close the descriptor, then check file data and zero-fill."},
  {"mmap-shared", mmap_shared,
   "Check shared writeback, file length, and the descriptor's read offset."},
  {"mmap-private", mmap_private,
   "Write to a private map backed by a read-only descriptor.\n  Verify that the file keeps its original bytes."},
  {"mmap-partial", mmap_partial,
   "Unmap the first page and short tail, then test the two middle pages."},
  {"mmap-dirty-pages", mmap_dirty_pages,
   "Write back a dirty page while preserving a separate write to a clean page."},
  {"mmap-invalid", mmap_invalid,
   "Reject invalid mmap and munmap calls and keep a valid mapping usable."},
  {"mmap-permissions", mmap_permissions,
   "Reject writes to read-only pages, both before and after a read."},
  {"mmap-unmapped-access", mmap_unmapped_access,
   "Read an address after munmap and check that the child traps."},
  {"mmap-slots", mmap_slots,
   "Fill the mapping table and check for overlap and reuse of free slots."},
  {"mmap-fork-private", mmap_fork_private,
   "Check that a forked child sees inherited bytes.\n  Verify that the parent and file keep their original bytes."},
  {"mmap-fork-exit", mmap_fork_exit,
   "Check child exit writeback and continued use of the parent's mapping."},
  {"mmap-private-exit", mmap_private_exit,
   "Exit with private writes and verify the file keeps its original bytes."},
  {"mmap-exec", mmap_exec,
   "Check mapping retention on failed exec and writeback on successful exec."},
  {"mmap-heap", mmap_heap,
   "Check that heap growth succeeds only after the mapping is removed."},
};

static void
clean_case(void)
{
  unlink("dir/link");
  unlink("dir/file");
  int fd = open_file(".", O_RDONLY);
  struct dirent entry;
  while (read(fd, &entry, sizeof(entry)) == sizeof(entry)) {
    if (entry.inum == 0)
      continue;
    char name[DIRSIZ + 1];
    memmove(name, entry.name, DIRSIZ);
    name[DIRSIZ] = 0;
    if (strcmp(name, ".") && strcmp(name, ".."))
      CHECK(unlink(name) == 0);
  }
  CHECK(close(fd) == 0);
}

int
main(int argc, char **argv)
{
  if (argc > 2) {
    fprintf(2, "usage: projecttest [test-name-prefix]\n");
    exit(1);
  }

  uint selected[sizeof(cases) / sizeof(cases[0])];
  int total = 0;
  uint prefix = argc == 2 ? strlen(argv[1]) : 0;
  for (uint i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
    if (prefix && (strlen(cases[i].name) < prefix || memcmp(cases[i].name, argv[1], prefix)))
      continue;
    selected[total++] = i;
  }
  if (total == 0) {
    fprintf(2, "projecttest: no tests matched %s\n", argv[1]);
    exit(1);
  }

  char directory[] = "/pt0000000000";
  uint pid = getpid();
  for (int i = sizeof(directory) - 2; pid != 0; i--, pid /= 10)
    directory[i] = '0' + pid % 10;
  CHECK(mkdir(directory) == 0);
  CHECK(chdir(directory) == 0);

  printf("Running %d project %s.\n", total, total == 1 ? "test" : "tests");
  int passed = 0, failed = 0;
  for (int i = 0; i < total; i++) {
    uint index = selected[i];
    printf("\nRUN  %s (%d/%d)\n  %s\n", cases[index].name, i + 1, total, cases[index].checks);
    uint start = uptime();
    int child = fork(), status;
    CHECK(child >= 0);
    if (child == 0) {
      cases[index].run();
      exit(0);
    }
    CHECK(wait(&status) == child);
    uint elapsed = (uint)uptime() - start;
    const char *unit = elapsed == 1 ? "tick" : "ticks";
    if (status == 0) {
      printf("PASS %s (%u %s)\n", cases[index].name, elapsed, unit);
      passed++;
    } else {
      printf("FAIL %s (%u %s, child status %d)\n", cases[index].name, elapsed, unit, status);
      failed++;
    }
    clean_case();
  }
  CHECK(chdir("/") == 0);
  CHECK(unlink(directory) == 0);
  printf("\nprojecttest: %s (%d passed, %d failed)\n",
         failed ? "FAILED" : "PASSED", passed, failed);
  exit(failed != 0);
}
