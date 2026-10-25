/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
#ifndef NR_CELL_INVENTORY_H
#define NR_CELL_INVENTORY_H
/* Cell inventory v1 (plan B.7, task D1): the file contract between the blind front end (writer) and the receiver (reader).
 * Spec: docs/superpowers/specs/mpci-cell-inventory-v1.md. This header: strict parser of that JSON into nr_cell_desc_t + a poll-based
 * atomic-file watcher. Pure and testable: no thread, no clock, no environment variable, no option. The path is an ARGUMENT (see
 * nr_cell_inventory_default_path); A8 wires it from the run directory.
 *
 * A file is accepted only if it parses completely and satisfies every rule of the spec (schema 1, all required keys, unique ids,
 * bounded counts). Anything else is rejected as a whole and the previously accepted inventory stays in force (a truncated file can
 * never replace a good one). Unknown keys are ignored (additive evolution inside v1). */
#include <stddef.h>
#include <stdint.h>
#include "nr_cell_desc.h"
#include "nr_cell_admit.h" /* NR_ADMIT_PLAN_MAX */
#ifdef __cplusplus
extern "C" {
#endif
#define NR_INV_ID_MAX 32          /* id string incl. NUL; longer ids are rejected */
#define NR_INV_FILE_MAX (1 << 20) /* larger files are rejected */

typedef struct {
  char id[NR_INV_ID_MAX];  /* stable key of a cell across updates */
  nr_cell_desc_t desc;     /* centre_hz, ssb_hz, gscn, ssb_index, pci, cfo_hz, n_rb (= n_rb_hint, 0 when null), scs_khz, n_rx (= window.n_rx),
                              window_centre_hz (= window.centre_hz), state; window = 0, weight = 1 */
  double sss_gap, dmrs_ratio; /* front-end detection quality (informational; 0 = unknown) */
  int64_t first_seen_ns;      /* front-end clock; informational */
} nr_inv_cell_t;

typedef struct {
  double window_centre_hz, window_rate_msps;
  int window_n_rx;
  int n;
  nr_inv_cell_t cell[NR_ADMIT_PLAN_MAX];
} nr_cell_inventory_t;

/* Parse `len` bytes of text. 0 = accepted (*out filled), -1 = rejected (*out unchanged; `err`, if not NULL, gets a one-line reason). */
int nr_cell_inventory_parse(const char *text, size_t len, nr_cell_inventory_t *out, char *err, size_t errn);
/* "<run_dir>/cells.json" -- the name the front-end tools already write (live_chain.sh: OUT/cells.json). 0, or -1 if it does not fit. */
int nr_cell_inventory_default_path(const char *run_dir, char *out, size_t n);
/* nr_cell_desc_state_t from the spec's state string ("new", "acquire", "maintain", "lost"); -1 if unknown. */
int nr_cell_inventory_state_from_name(const char *s);
const char *nr_cell_inventory_state_name(int state);

typedef struct {
  char path[512];
  int have_stat;                       /* a file was seen at least once */
  int64_t mtime_sec, mtime_nsec, size, ino; /* (mtime, size, inode) of the last file read (accepted or rejected) */
  int have_inv;                        /* an inventory was accepted */
  unsigned generation;                 /* number of accepted inventories */
  nr_cell_inventory_t inv;             /* the accepted inventory */
} nr_cell_inventory_watch_t;

/* 0, or -1 if the path does not fit. */
int nr_cell_inventory_watch_init(nr_cell_inventory_watch_t *w, const char *path);
/* Re-read the file iff (mtime, size, inode) differ from the last read. Returns
 *   1  a new inventory was accepted (w->inv, w->generation updated),
 *   0  nothing to do (file unchanged, or not there yet),
 *  -1  the file changed but was rejected (truncated, invalid, unreadable): previous inventory kept; `err` says why.
 * A rejected file is not re-parsed until its stat changes again. */
int nr_cell_inventory_poll(nr_cell_inventory_watch_t *w, char *err, size_t errn);
#ifdef __cplusplus
}
#endif
#endif
