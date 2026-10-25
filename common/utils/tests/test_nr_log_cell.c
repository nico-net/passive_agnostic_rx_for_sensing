/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
/* D2: per-cell line prefix. N=1 output has no prefix and is byte-identical to printf; N=2 prefixes every line with the
 * cell of the calling thread, for LOG_x() records and for the nr_cprintf shim. Output is captured by redirecting fd 1. */
#define _GNU_SOURCE
#include <assert.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "common/config/config_load_configmodule.h"
#include "common/utils/LOG/log.h"
#include "common/utils/nr_cprintf.h"

static int g_saved = -1, g_tmp = -1;
static char g_path[] = "/tmp/test_nr_log_cell_XXXXXX";

static void cap_begin(void)
{
  fflush(stdout);
  g_saved = dup(1);
  g_tmp = mkstemp(g_path);
  assert(g_tmp >= 0);
  dup2(g_tmp, 1);
  if (ftruncate(g_tmp, 0)) {}
  lseek(g_tmp, 0, SEEK_SET);
}

static char *cap_end(void)
{
  fflush(stdout);
  dup2(g_saved, 1);
  close(g_saved);
  static char buf[1 << 16];
  lseek(g_tmp, 0, SEEK_SET);
  ssize_t n = read(g_tmp, buf, sizeof(buf) - 1);
  assert(n >= 0);
  buf[n] = 0;
  close(g_tmp);
  unlink(g_path);
  strcpy(g_path, "/tmp/test_nr_log_cell_XXXXXX");
  return buf;
}

/* the message part of a LOG line: from "SENSING:" to the end of that line (header/time/colour are not compared) */
static int count_lines_with(const char *out, const char *needle, const char *prefix_expect)
{
  int n = 0;
  for (const char *l = out; *l;) {
    const char *e = strchr(l, '\n');
    size_t ll = e ? (size_t)(e - l) : strlen(l);
    char line[4096];
    snprintf(line, sizeof(line), "%.*s", (int)ll, l);
    /* with colours on, the previous record's end sequence (ESC[0m) is the first thing on this line: skip it, as the parsers do */
    char *lp = line;
    while (lp[0] == 0x1b && lp[1] == '[' && strchr(lp, 'm')) lp = strchr(lp, 'm') + 1;
    if (strstr(lp, needle)) {
      n++;
      if (prefix_expect)
        assert(strncmp(lp, prefix_expect, strlen(prefix_expect)) == 0);
      else
        assert(strstr(lp, "[c0]") == NULL && strstr(lp, "[c1]") == NULL);
    }
    l += ll + (e ? 1 : 0);
  }
  return n;
}

static void *cell1_thread(void *a)
{
  (void)a;
  assert(nr_cell_idx() == 1);
  LOG_I(PHY, "SENSING: PROBE_LOG cell1\n");
  nr_cprintf("SENSING: PROBE_PRINTF cell1 %d\n", 7);
  return NULL;
}

int main(int argc, char **argv)
{
  configmodule_interface_t *cfg = load_configmodule(argc, argv, CONFIG_ENABLECMDLINEONLY);
  assert(cfg != NULL);
  logInit();
  set_log(PHY, OAILOG_INFO);
  /* ---- N = 1: no prefix at all ---- */
  assert(nr_cell_count() == 1);
  cap_begin();
  LOG_I(PHY, "SENSING: PROBE_LOG single\n");
  nr_cprintf("SENSING: PROBE_PRINTF single %d\n", 7);
  nr_cprintf("two\nlines\n");
  char *o = cap_end();
  assert(count_lines_with(o, "PROBE_LOG single", NULL) == 1);
  /* nr_cprintf at N=1 is printf: exact bytes */
  assert(strstr(o, "SENSING: PROBE_PRINTF single 7\ntwo\nlines\n") != NULL);
  assert(strstr(o, "[c") == NULL);

  /* ---- N = 1 set explicitly (the cell manager may call it with 1): still no prefix ---- */
  nr_cell_set_count(1);
  cap_begin();
  LOG_I(PHY, "SENSING: PROBE_LOG explicit1\n");
  o = cap_end();
  assert(count_lines_with(o, "PROBE_LOG explicit1", NULL) == 1);

  /* ---- N = 2: prefix per thread cell ---- */
  nr_cell_set_count(2);
  cap_begin();
  LOG_I(PHY, "SENSING: PROBE_LOG cell0\n");
  nr_cprintf("SENSING: PROBE_PRINTF cell0 %d\n", 7);
  pthread_t t;
  assert(nr_cell_thread_create(&t, NULL, cell1_thread, NULL, 1) == 0);
  pthread_join(t, NULL);
  LOG_I(PHY, "SENSING: MULTI line one\nSENSING: MULTI line two\n"); /* a record with two lines: both tagged */
  nr_cprintf("a\nb\n");
  o = cap_end();
  assert(count_lines_with(o, "PROBE_LOG cell0", "[c0] ") == 1);
  assert(count_lines_with(o, "PROBE_PRINTF cell0 7", "[c0] ") == 1);
  assert(count_lines_with(o, "PROBE_LOG cell1", "[c1] ") == 1);
  assert(count_lines_with(o, "PROBE_PRINTF cell1 7", "[c1] ") == 1);
  assert(count_lines_with(o, "MULTI line one", "[c0] ") == 1);
  assert(count_lines_with(o, "MULTI line two", "[c0] ") == 1);
  assert(strstr(o, "[c0] a\n[c0] b\n") != NULL);
  /* no tag stacking: exactly one tag per line */
  assert(strstr(o, "[c0] [c0]") == NULL && strstr(o, "[c1] [c1]") == NULL);

  /* ---- pure helper: truncation never overruns, trailing newline not tagged ---- */
  {
    char b[32] = "x\ny\n";
    size_t n = nr_cell_tag_lines(b, 4, sizeof(b), 3);
    assert(n == 4 + 2 * 5 && memcmp(b, "[c3] x\n[c3] y\n", n) == 0);
    char c[8] = "abcdef\n";
    n = nr_cell_tag_lines(c, 7, sizeof(c), 3);
    assert(n == 8 && memcmp(c, "[c3] abc", n) == 0);
  }
  printf("test_nr_log_cell: PASS\n");
  return 0;
}
