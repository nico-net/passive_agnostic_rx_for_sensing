/* Per-cell instance tag for the multi-PCI deployment (one receiver process per cell, shm IQ ring).
 * ISAC_CELL_INSTANCE=<tag> ([A-Za-z0-9_-], 1..32 chars) turns every host-global default path
 * (/tmp/..., cwd logs) into a per-tag one. Unset: every path is returned unchanged (byte-identical).
 * An invalid tag is FATAL (exit 2): silently falling back would make two cells share files.
 * Header-only and stateless (getenv per call), so there is no cache to leak across fork(). */
#ifndef NR_CELL_INSTANCE_H
#define NR_CELL_INSTANCE_H
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#define NR_CELL_TAG_MAX 32

static inline const char *nr_cell_instance(void)
{
  const char *t = getenv("ISAC_CELL_INSTANCE");
  if (t == NULL || *t == 0)
    return NULL;
  size_t n = strlen(t);
  int ok = n <= NR_CELL_TAG_MAX;
  for (size_t i = 0; ok && i < n; i++)
    ok = (t[i] >= 'a' && t[i] <= 'z') || (t[i] >= 'A' && t[i] <= 'Z') || (t[i] >= '0' && t[i] <= '9') || t[i] == '_' || t[i] == '-';
  if (!ok) {
    fprintf(stderr, "ISAC_CELL_INSTANCE='%s' invalid ([A-Za-z0-9_-], 1..%d chars): refusing to share host-global files\n", t, NR_CELL_TAG_MAX);
    exit(2);
  }
  return t;
}

/* Default host-global path -> per-instance path. File with extension: /tmp/a.txt -> /tmp/a.<tag>.txt;
 * file without extension or directory: /tmp/passive_rx -> /tmp/passive_rx/<tag>. Unset: copy unchanged.
 * Callers must not pass a user-chosen explicit path (explicit env overrides win untouched). */
static inline void nr_cell_path(const char *def, char *out, size_t n)
{
  const char *tag = nr_cell_instance();
  if (tag == NULL) {
    snprintf(out, n, "%s", def);
    return;
  }
  const char *slash = strrchr(def, '/');
  const char *dot = strrchr(slash ? slash : def, '.');
  if (dot != NULL && dot != (slash ? slash + 1 : def))
    snprintf(out, n, "%.*s.%s%s", (int)(dot - def), def, tag, dot);
  else
    snprintf(out, n, "%s/%s", def, tag);
}

/* mkdir -p for a directory (mode 0777 & umask), tolerant of existing dirs. */
static inline void nr_cell_mkdirs(const char *dir)
{
  char buf[512];
  snprintf(buf, sizeof(buf), "%s", dir);
  for (char *p = buf + 1; *p; p++)
    if (*p == '/') {
      *p = 0;
      mkdir(buf, 0777);
      *p = '/';
    }
  mkdir(buf, 0777);
}
#endif
