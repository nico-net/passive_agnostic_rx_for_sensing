/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
#define _GNU_SOURCE
#include "raw_iq_source.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <unistd.h>

int main(void)
{
  /* Run unprivileged: CAP_IPC_LOCK would bypass the falsifiable lock limit. */
  if (geteuid() == 0) { fputs("VOID: run this test unprivileged\n", stderr); return 77; }
  const uint64_t samples = 128 * 1024 * 1024;
  char dir[] = "/tmp/raw-iq-memory.XXXXXX", path[128];
  assert(mkdtemp(dir));
  snprintf(path, sizeof(path), "%s/rx0.sc16", dir);
  int fd = open(path, O_CREAT | O_EXCL | O_RDWR, 0600);
  assert(fd >= 0 && ftruncate(fd, (off_t)samples * 4) == 0);
  int16_t first[] = {17, -23}, last[] = {122, -322}, output[2];
  assert(pwrite(fd, first, sizeof(first), 0) == sizeof(first));
  assert(pwrite(fd, last, sizeof(last), (off_t)(samples - 1) * 4) == sizeof(last));
  assert(close(fd) == 0);
  struct rlimit limit;
  assert(getrlimit(RLIMIT_MEMLOCK, &limit) == 0);
  if (limit.rlim_max < 65536) {
    unlink(path); rmdir(dir);
    fputs("VOID: insufficient memlock allowance to establish precondition\n", stderr);
    return 77;
  }
  limit.rlim_cur = 65536;
  assert(setrlimit(RLIMIT_MEMLOCK, &limit) == 0);
  assert(mlockall(MCL_FUTURE) == 0);
  raw_iq_source source;
  int rc = raw_iq_open(&source, dir, samples, 4567, 1, 122880000, 3450000000, 0);
  int saved = errno;
  if (rc != 0) {
    munlockall(); unlink(path); rmdir(dir);
    fprintf(stderr, "FAIL: replay inherited MCL_FUTURE: %s\n", strerror(saved));
    return 1;
  }
  void *buffers[] = {output};
  uint64_t timestamp;
  assert(raw_iq_read(&source, buffers, 1, 1, &timestamp) == 1);
  assert(timestamp == 4567 && !memcmp(output, first, sizeof(first)));
  source.position = samples - 1;
  assert(raw_iq_read(&source, buffers, 1, 1, &timestamp) == 1);
  assert(timestamp == 4567 + samples - 1 && !memcmp(output, last, sizeof(last)));
  raw_iq_close(&source);
  unlink(path); rmdir(dir);
  puts("PASS: 512 MiB replay under 64 KiB memlock limit after MCL_FUTURE; samples/timestamps preserved");
  return 0;
}
