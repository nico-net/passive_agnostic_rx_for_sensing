/* Fake receiver for test_nr_dashboard.py: nr_dashboard_start() against a fake run directory. argv: run_dir port core monitor.py */
#include "../nr_dashboard.h"
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv)
{
  if (argc != 5)
    return 2;
  nr_dashboard_cfg_t c = {.run_dir = argv[1], .port = atoi(argv[2]), .core = atoi(argv[3]), .script = argv[4]};
  char err[200] = "", url[64], run[4096];
  int r = nr_dashboard_start(&c, err, sizeof(err), url, sizeof(url), run, sizeof(run));
  printf("%d %s %s %s\n", r, url, run, err);
  return r ? 1 : 0;
}
