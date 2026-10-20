#ifndef NR_DASHBOARD_H
#define NR_DASHBOARD_H
/* Always-on receiver dashboard: tests/passive_rx/monitor/monitor.py started detached, as the invoking user, pinned to one spare core at nice 19 / SCHED_IDLE. */

typedef struct {
  const char *run_dir;      /* run directory (NULL: derived from stdout when it is a file) */
  const char *log_path;     /* receiver log (NULL: stdout file) */
  const char *metrics_path; /* ISAC_METRICS_PATH file or NULL */
  const char *script;       /* monitor.py (NULL: searched upward from /proc/self/exe) */
  const char *runs_glob;    /* followed when no run directory is known (NULL: ~/NICOLA/ota_runs, newest dir) */
  int port;                 /* 0: 8080 */
  int core;                 /* CPU to pin to; < 0: 2 (spare core of the site core map) */
} nr_dashboard_cfg_t;

/* Fork-detach-exec the dashboard. Returns 0 when launched (the monitor itself keeps a single instance per port and reuses one that already
 * serves the same run), -1 on failure (reason in err); the receiver never depends on the outcome. out_url/out_run describe what is served. */
int nr_dashboard_start(const nr_dashboard_cfg_t *cfg, char *err, int errlen, char *out_url, int urllen, char *out_run, int runlen);
#endif
