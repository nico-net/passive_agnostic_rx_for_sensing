/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
/* nr_gpu_cell_bytes: per-cell GPU / pinned-host bytes by kind (see nr_gpu_cell.h). Pure C, no CUDA: also linked into the no-GPU (cloud) builds, where it is
 * the same arithmetic as on the DGX, so the admission model has no stub value to disagree with. */
#include "nr_gpu_cell.h"
#include "nr_csirs_disc_mem.h"

nr_gpu_cell_mem_t nr_gpu_cell_mem(const nr_gpu_cell_params_t *p, int kind)
{
  nr_gpu_cell_mem_t m = {0, 0};
  if (!p) return m;
  const nr_gpu_cell_geom_t g = nr_gpu_cell_geom(p);
  const int dur = p->nid_dur > 0 ? p->nid_dur : NR_GPU_CELL_NID_DUR_DEFAULT, win = p->nid_window > 0 ? p->nid_window : NR_GPU_CELL_NID_WINDOW_DEFAULT;
  switch (kind) {
    case NR_GPU_CELL_HARQ:
      m.dev = (size_t)NR_GPU_CELL_HQ_BLK * NR_GPU_CELL_HQ_SEG * 2;
      break;
    case NR_GPU_CELL_PDSCH_FEP: /* staging h_rx, h_rot, h_check (pin); d_rx, d_fft_in, d_rxF, d_rot (dev) */
      m.pin = nr_gpu_cell_fep_rx_bytes(g.n_rx, g.sps, g.fft) + nr_gpu_cell_fep_rot_bytes() + nr_gpu_cell_fep_check_bytes(g.ns, g.fft);
      m.dev = nr_gpu_cell_fep_rx_bytes(g.n_rx, g.sps, g.fft) + 2 * nr_gpu_cell_fep_fft_bytes(g.n_rx, g.ns, g.fft) + nr_gpu_cell_fep_rot_bytes();
      break;
    case NR_GPU_CELL_PDSCH_FEP_EXACT: /* d_exact + h_exact, and per pass slot a staging ring and its device copy */
      m.dev = nr_gpu_cell_fep_exact_bytes(g.n_rx, g.fft) + (size_t)NR_GPU_CELL_PASS_SLOTS * nr_gpu_cell_fep_rx_bytes(g.n_rx, g.sps, g.fft);
      m.pin = m.dev;
      break;
    case NR_GPU_CELL_PDCCH_FEP:
      m.dev = m.pin = nr_gpu_cell_pdcch_rxf_bytes(g.n_rx, g.fft);
      break;
    case NR_GPU_CELL_NID_WINDOW:
      m.dev = nr_gpu_cell_nid_window_dev_bytes(g.n_rb, dur, win);
      break;
    case NR_GPU_CELL_NID_STAGING:
      m.pin = nr_gpu_cell_nid_staging_bytes(g.n_rb, dur);
      break;
    case NR_GPU_CELL_CSIRS_FIXED: {
      const size_t stride = 14 * ((size_t)g.fft + (size_t)(g.fft / 128 * 9) + (size_t)(g.fft / 128 * (9 + (1 << g.mu))));
      m.dev = nr_csirs_disc_mem_fixed(g.n_rb, g.n_rx, g.fft, stride, NR_GPU_CELL_CSIRS_BATCH);
      break;
    }
    case NR_GPU_CELL_CSIRS_SLOT:
      m.dev = nr_csirs_disc_mem_per_slot(g.n_rb, g.n_rx);
      break;
    case NR_GPU_SHARED_NID_FIXED:
      m.dev = nr_gpu_cell_nid_fixed_bytes();
      break;
    default:
      break;
  }
  return m;
}
size_t nr_gpu_cell_bytes(const nr_gpu_cell_params_t *p, int kind)
{
  const nr_gpu_cell_mem_t m = nr_gpu_cell_mem(p, kind);
  return m.dev + m.pin;
}
const char *nr_gpu_cell_kind_name(int kind)
{
  static const char *const n[NR_GPU_CELL_KIND_N] = {"harq", "pdsch_fep", "pdsch_fep_exact", "pdcch_fep", "nid_window", "nid_staging", "csirs_fixed", "csirs_slot", "nid_fixed"};
  return kind >= 0 && kind < NR_GPU_CELL_KIND_N ? n[kind] : "?";
}
