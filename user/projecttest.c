#include "kernel/types.h"
#include "kernel/fcntl.h"
#include "user/user.h"

static volatile int alarm_count;

static void
alarm_handler(void)
{
  alarm_count++;
  sigreturn();
}

static int
check_alarm(void)
{
  volatile int work = 0;

  alarm_count = 0;
  if (sigalarm(1, alarm_handler) < 0)
    return -1;
  for (int i = 0; i < 20000000; i++)
    work += i;
  sigalarm(0, 0);
  printf("alarm callbacks: %d (work=%d)\n", alarm_count, work == -1);
  return alarm_count > 0 ? 0 : -1;
}

static int
check_large_file(void)
{
  char block[1024];
  int fd = open("project_large", O_CREATE | O_RDWR | O_TRUNC);
  if (fd < 0)
    return -1;
  memset(block, 'L', sizeof(block));
  for (int i = 0; i < 270; i++)
    if (write(fd, block, sizeof(block)) != sizeof(block)) {
      close(fd);
      return -1;
    }
  close(fd);

  fd = open("project_large", O_RDONLY);
  if (fd < 0)
    return -1;
  if (read(fd, block, sizeof(block)) != sizeof(block) ||
      block[0] != 'L') {
    close(fd);
    return -1;
  }
  close(fd);
  printf("large file: 270 blocks written and read\n");
  return 0;
}

static int
check_symlink_and_mmap(void)
{
  char block[1024];
  int fd = open("project_map_target", O_CREATE | O_RDWR | O_TRUNC);
  if (fd < 0)
    return -1;
  memset(block, 'M', sizeof(block));
  if (write(fd, block, sizeof(block)) != sizeof(block)) {
    close(fd);
    return -1;
  }
  close(fd);

  unlink("project_link");
  if (symlink("project_map_target", "project_link") < 0)
    return -1;
  fd = open("project_link", O_RDONLY);
  if (fd < 0)
    return -1;
  if (read(fd, block, sizeof(block)) != sizeof(block) || block[0] != 'M') {
    close(fd);
    return -1;
  }
  close(fd);

  fd = open("project_map_target", O_RDWR);
  if (fd < 0)
    return -1;
  char *mapped = mmap(0, 2 * 1024, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (mapped == (char *)-1)
    return -1;
  if (mapped[0] != 'M')
    return -1;
  mapped[0] = 'Z';
  if (munmap(mapped, 2 * 1024) < 0)
    return -1;
  close(fd);

  fd = open("project_map_target", O_RDONLY);
  if (fd < 0 || read(fd, block, sizeof(block)) != sizeof(block) ||
      block[0] != 'Z') {
    close(fd);
    return -1;
  }
  close(fd);
  printf("symlink and mmap: passed\n");
  return 0;
}

int
main(void)
{
  int failed = 0;

  if (check_alarm() < 0) {
    printf("alarm: FAILED\n");
    failed = 1;
  }
  if (check_large_file() < 0) {
    printf("large file: FAILED\n");
    failed = 1;
  }
  if (check_symlink_and_mmap() < 0) {
    printf("symlink and mmap: FAILED\n");
    failed = 1;
  }
  printf("projecttest: %s\n", failed ? "FAILED" : "PASSED");
  exit(failed);
}
