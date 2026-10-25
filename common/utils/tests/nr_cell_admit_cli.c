/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
/* Fixture runner for nr_cell_admit_plan: reads a text fixture and prints one line per cell, so tests/passive_rx/mpci/admit_model.py can be compared with the C function
 * on the same cases (test_admit_model.py). Fixture grammar (one record per line, '#' comments):
 *   case <name>
 *   caps key=value ...   mem_avail_mb (-1 unknown), shared_mb, tier, unified, gpu_free_mb (0 unknown), gpu_shared_mb, max_cells, chan_msps, consumers, depth, csirs, recovery, gw
 *   cell key=value ...   rb, scs, rx, centre, wcentre, state, weight
 *   end
 * Output: "<case> <i> served=<0|1> reason=<name> order=<k> snap=<n> csirs=<n> td=<n> gw=<n> gslots=<n> host=<bytes> gpu=<bytes>" per cell, then
 * "<case> plan served=<n> refuse=<0|1> binding=<name> hused=<bytes> gused=<bytes> nid=<n> graph=<bytes>". */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nr_cell_admit.h"

static void kv(const char *tok, nr_host_caps_t *c, nr_cell_desc_t *d)
{
  char key[32];
  double v;
  if (sscanf(tok, "%31[^=]=%lf", key, &v) != 2) return;
  if (c) {
    if (!strcmp(key, "mem_avail_mb")) c->mem_avail = v < 0 ? SIZE_MAX : (size_t)v << 20;
    else if (!strcmp(key, "shared_mb")) c->host_shared_bytes = (size_t)v << 20;
    else if (!strcmp(key, "tier")) c->gpu_tier = (int)v;
    else if (!strcmp(key, "unified")) c->unified = (int)v;
    else if (!strcmp(key, "gpu_free_mb")) c->gpu_free_bytes = (size_t)v << 20;
    else if (!strcmp(key, "gpu_shared_mb")) c->gpu_shared_bytes = (size_t)v << 20;
    else if (!strcmp(key, "max_cells")) c->max_cells = (int)v;
    else if (!strcmp(key, "chan_msps")) c->chan_budget_msps = v;
    else if (!strcmp(key, "consumers")) c->consumers = (int)v;
    else if (!strcmp(key, "depth")) c->depth = (int)v;
    else if (!strcmp(key, "csirs")) c->csirs_enabled = (int)v;
    else if (!strcmp(key, "recovery")) c->recovery_enabled = (int)v;
    else if (!strcmp(key, "gw")) c->gw_enabled = (int)v;
  }
  if (d) {
    if (!strcmp(key, "rb")) d->n_rb = (int)v;
    else if (!strcmp(key, "scs")) d->scs_khz = (int)v;
    else if (!strcmp(key, "rx")) d->n_rx = (int)v;
    else if (!strcmp(key, "centre")) d->centre_hz = v;
    else if (!strcmp(key, "wcentre")) d->window_centre_hz = v;
    else if (!strcmp(key, "state")) d->state = (int)v;
    else if (!strcmp(key, "weight")) d->weight = (int)v;
  }
}

static void run(const char *name, const nr_host_caps_t *caps, const nr_cell_desc_t *cells, int n)
{
  static nr_admit_plan_t p;
  const int ns = nr_cell_admit_plan(caps, cells, n, &p);
  for (int i = 0; i < n; i++) {
    const nr_admit_cell_t *c = &p.cell[i];
    printf("%s %d served=%d reason=%s order=%d snap=%d csirs=%d td=%d gw=%zu gslots=%d host=%zu gpu=%zu\n", name, i, c->served, nr_admit_reason_name(c->reason), c->order, c->snap_holders,
           c->csirs_slots, c->td_slots, c->gw_buffers, c->gpu_csirs_slots, c->host_bytes, c->gpu_bytes);
  }
  printf("%s plan served=%d refuse=%d binding=%s hused=%zu gused=%zu nid=%d graph=%zu\n", name, ns, p.refuse, nr_admit_reason_name(p.binding), p.host_used == SIZE_MAX ? 0 : p.host_used,
         p.gpu_used == SIZE_MAX ? 0 : p.gpu_used, p.nid_window, p.gpu_graph_bytes);
}

int main(int argc, char **argv)
{
  FILE *f = argc > 1 ? fopen(argv[1], "r") : stdin;
  if (!f) { perror("fixture"); return 2; }
  char line[1024], name[64] = "";
  nr_host_caps_t caps;
  static nr_cell_desc_t cells[NR_ADMIT_PLAN_MAX];
  int n = 0;
  while (fgets(line, sizeof line, f)) {
    char *tok = strtok(line, " \t\r\n");
    if (!tok || tok[0] == '#') continue;
    if (!strcmp(tok, "case")) {
      tok = strtok(NULL, " \t\r\n");
      snprintf(name, sizeof name, "%s", tok ? tok : "?");
      memset(&caps, 0, sizeof caps);
      caps.mem_avail = SIZE_MAX; caps.max_cells = NR_ADMIT_UNLIMITED_CELLS; caps.chan_budget_msps = -1; caps.consumers = 4; caps.depth = 64;
      n = 0;
    } else if (!strcmp(tok, "caps")) {
      while ((tok = strtok(NULL, " \t\r\n"))) kv(tok, &caps, NULL);
    } else if (!strcmp(tok, "cell") && n < NR_ADMIT_PLAN_MAX) {
      nr_cell_desc_init(&cells[n]);
      cells[n].scs_khz = 30;
      while ((tok = strtok(NULL, " \t\r\n"))) kv(tok, NULL, &cells[n]);
      n++;
    } else if (!strcmp(tok, "end")) {
      run(name, &caps, cells, n);
    }
  }
  return 0;
}
