#define _GNU_SOURCE
#include "nr_dashboard.h"
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <libgen.h>
#include <limits.h>
#include <pwd.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define ERR(...) (err ? (void)snprintf(err, errlen, __VA_ARGS__) : (void)0)

static int find_script(char *out, size_t n)
{
  char exe[PATH_MAX];
  ssize_t l = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
  if (l <= 0)
    return -1;
  exe[l] = 0;
  for (char *d = dirname(exe); strlen(d) > 1; d = dirname(d)) { /* the repository is a few levels above the build directory */
    snprintf(out, n, "%s/tests/passive_rx/monitor/monitor.py", d);
    if (access(out, R_OK) == 0)
      return 0;
  }
  return -1;
}

int nr_dashboard_start(const nr_dashboard_cfg_t *cfg, char *err, int errlen, char *out_url, int urllen, char *out_run, int runlen)
{
  char logp[PATH_MAX] = "", rund[PATH_MAX] = "", script[PATH_MAX] = "", port[16], core_s[16], dlog[PATH_MAX + 20], glob[PATH_MAX];
  struct stat st;
  const int port_n = cfg->port > 0 ? cfg->port : 8080, core = cfg->core >= 0 ? cfg->core : 2;
  snprintf(port, sizeof(port), "%d", port_n);
  snprintf(core_s, sizeof(core_s), "%d", core);

  if (cfg->log_path)
    snprintf(logp, sizeof(logp), "%s", cfg->log_path);
  else if (!cfg->run_dir) { /* an explicit run directory brings its own run.log */
    ssize_t l = readlink("/proc/self/fd/1", logp, sizeof(logp) - 1);
    logp[l > 0 ? l : 0] = 0;
    if (!logp[0] || logp[0] != '/' || stat(logp, &st) || !S_ISREG(st.st_mode))
      logp[0] = 0; /* stdout is a pipe, tty or deleted file */
  }
  if (cfg->run_dir)
    snprintf(rund, sizeof(rund), "%s", cfg->run_dir);
  else if (logp[0]) {
    char t[PATH_MAX];
    snprintf(t, sizeof(t), "%s", logp);
    snprintf(rund, sizeof(rund), "%s", dirname(t));
  }

  uid_t uid = getuid(), gid = getgid(); /* target identity: never root */
  if (geteuid() == 0) {
    const char *su = getenv("SUDO_UID"), *sg = getenv("SUDO_GID");
    if (su && sg && atoi(su) > 0) {
      uid = (uid_t)atoi(su);
      gid = (uid_t)atoi(sg);
    } else if (rund[0] && !stat(rund, &st) && st.st_uid > 0) { /* invoking user = owner of the run directory */
      uid = st.st_uid;
      gid = st.st_gid;
    } else {
      ERR("root without SUDO_UID and no user-owned run directory: dashboard not started");
      return -1;
    }
  }
  if (!rund[0]) { /* stdout is not a file: follow the newest run under the runs root */
    const char *g = cfg->runs_glob;
    if (!g) {
      struct passwd *pw = getpwuid(uid);
      snprintf(glob, sizeof(glob), "%s/NICOLA/ota_runs/*", pw ? pw->pw_dir : "/tmp");
      g = glob;
    }
    snprintf(rund, sizeof(rund), "%s", g);
  }
  if (cfg->script)
    snprintf(script, sizeof(script), "%s", cfg->script);
  else if (find_script(script, sizeof(script))) {
    ERR("monitor.py not found above the executable");
    return -1;
  }
  const char *py = access("/usr/bin/python3", X_OK) == 0 ? "/usr/bin/python3" : "/usr/local/bin/python3";
  if (access(py, X_OK)) {
    ERR("python3 not found");
    return -1;
  }
  int follow = !logp[0] && !cfg->run_dir && strchr(rund, '*');
  snprintf(dlog, sizeof(dlog), "%s/dashboard.log", follow ? "/tmp" : rund);
  const char *argv[20];
  int n = 0;
  argv[n++] = "python3";
  argv[n++] = script;
  argv[n++] = "--single";
  argv[n++] = "--port";
  argv[n++] = port;
  argv[n++] = follow ? "--follow" : "--run";
  argv[n++] = rund;
  if (logp[0]) {
    argv[n++] = "--log";
    argv[n++] = logp;
  }
  if (cfg->metrics_path && cfg->metrics_path[0]) {
    argv[n++] = "--metrics";
    argv[n++] = cfg->metrics_path;
  }
  argv[n] = NULL;

  const int dropping = geteuid() == 0;
  gid_t g1 = (gid_t)gid;
  pid_t p = fork();
  if (p < 0) {
    ERR("fork: %s", strerror(errno));
    return -1;
  }
  if (p == 0) { /* only async-signal-safe calls from here: the receiver is multi-threaded */
    pid_t q = fork();
    if (q != 0)
      _exit(q < 0 ? 1 : 0); /* double fork: the receiver never has to reap the dashboard */
    setsid();
    sigset_t all;
    sigemptyset(&all);
    sigprocmask(SIG_SETMASK, &all, NULL);
    for (int s = 1; s < 32; s++)
      if (s != SIGKILL && s != SIGSTOP)
        signal(s, SIG_DFL);
    int dn = open("/dev/null", O_RDWR);
    if (dn >= 0) {
      dup2(dn, 0);
      dup2(dn, 1);
      dup2(dn, 2);
    }
    syscall(SYS_close_range, 3, ~0U, 0); /* no GPU / radio / socket fds */
    cpu_set_t cs;
    CPU_ZERO(&cs);
    CPU_SET(core, &cs);
    sched_setaffinity(0, sizeof(cs), &cs);
    struct sched_param sp = {0};
    sched_setscheduler(0, SCHED_IDLE, &sp); /* idle class; nice 19 below is the fallback */
    setpriority(PRIO_PROCESS, 0, 19);
    struct rlimit rl = {4UL << 30, 4UL << 30}; /* bounded memory */
    setrlimit(RLIMIT_AS, &rl);
    if (dropping && (setgroups(1, &g1) || setgid(gid) || setuid(uid) || setuid(0) == 0))
      _exit(2); /* privileges not dropped (or regainable): never run python as root */
    int lf = open(dlog, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    if (lf >= 0) {
      dup2(lf, 1);
      dup2(lf, 2);
    }
    execv(py, (char *const *)argv);
    _exit(3);
  }
  int status = 0;
  waitpid(p, &status, 0);
  if (!WIFEXITED(status) || WEXITSTATUS(status)) {
    ERR("fork of the dashboard failed");
    return -1;
  }
  if (out_url)
    snprintf(out_url, urllen, "http://127.0.0.1:%d", port_n);
  if (out_run)
    snprintf(out_run, runlen, "%s", rund);
  return 0;
}
