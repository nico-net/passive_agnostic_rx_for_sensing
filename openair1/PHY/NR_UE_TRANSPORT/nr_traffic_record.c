/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
/* Traffic-mode DCI record: JSON formatter (writer thread) and the emit functions. */
#include "nr_traffic_record.h"
#include "nr_sense_api.h"
#include <inttypes.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

typedef struct { char *p; size_t n, w; bool bad; } tj_t;
static void tput(tj_t *j, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void tput(tj_t *j, const char *fmt, ...)
{
  if (j->bad) return;
  va_list ap; va_start(ap, fmt);
  const int k = vsnprintf(j->p + j->w, j->n - j->w, fmt, ap);
  va_end(ap);
  if (k < 0 || (size_t)k >= j->n - j->w) j->bad = true; else j->w += (size_t)k;
}
static void tint(tj_t *j, const char *key, int64_t v)   /* -1 (any negative) = unknown = null */
{
  if (v < 0) tput(j, ",\"%s\":null", key); else tput(j, ",\"%s\":%" PRId64, key, v);
}

int nr_sense_dci_to_json(const nr_sense_dci_rec_t *r, char *buf, size_t n)
{
  if (!r || !buf || !n) return -1;
  static const char *const lname[] = {"fixed", "solved", "candidate", "unsolved"};
  const bool nolayout = r->layout_state == NR_LAYOUT_CANDIDATE || r->layout_state == NR_LAYOUT_UNSOLVED;
  nr_passive_obs_t o = r->o;
  if (nolayout) o.start_rb = o.nb_rb = o.start_sym = o.nb_sym = o.mcs = o.mcs_table = o.qm = o.nl = o.tbs =
      o.harq_pid = o.ndi = o.rv = -1;
  char g[2048];
  if (nr_sense_grant_to_json(&o, &r->m, g, sizeof g) < 0) return -1;
  char *t = strstr(g, "\"type\":\"grant\"");
  const size_t gl = strlen(g);
  if (!t || gl < 2) return -1;
  tj_t j = {buf, n, 0, false};
  tput(&j, "%.*s\"type\":\"dci\"%.*s", (int)(t - g), g, (int)(gl - 1 - (size_t)(t - g) - 14), t + 14);
  tint(&j, "dci_format", r->dci_format); tint(&j, "ss_type", r->ss_type); tint(&j, "al", r->al);
  tint(&j, "cce", r->cce); tint(&j, "coreset_id", r->coreset_id); tint(&j, "dci_len", r->dci_len);
  tput(&j, ",\"dci_bits\":\"");
  for (unsigned i = 0; i < (r->dci_len + 7u) / 8u && i < sizeof r->dci_bits; i++) tput(&j, "%02x", r->dci_bits[i]);
  tput(&j, "\",\"rnti_confirmed\":%s,\"layout_state\":\"%s\"", r->rnti_confirmed ? "true" : "false",
       lname[(unsigned)r->layout_state & 3u]);
  tput(&j, ",\"layout_checks_ok\":%u,\"layout_checks_fail\":%u", r->layout_checks_ok, r->layout_checks_fail);
  if (r->m.identity_gen < 0 || r->m.dedicated_epoch < 0) tput(&j, ",\"cfg_ref\":null");
  else tput(&j, ",\"cfg_ref\":\"%d.%d.%u\"", r->m.identity_gen, r->m.dedicated_epoch, r->cfg_ref_ver);
  tint(&j, "bwp_start", r->bwp_start); tint(&j, "bwp_size", r->bwp_size); tint(&j, "start_rb_bwp", r->start_rb_bwp);
#define L(k, f) tint(&j, k, nolayout ? -1 : r->f)
  L("vrb_prb", vrb_prb); L("tda_index", tda_index); L("k0", k0); L("k1", k1); L("dai", dai); L("tpc", tpc); L("pri", pri);
#undef L
  tint(&j, "srs_req", r->srs_req); tint(&j, "csi_req", r->csi_req); tint(&j, "ulsch_ind", r->ulsch_ind);
  tint(&j, "cbgti", r->cbgti); tint(&j, "retx", nolayout ? -1 : r->retx);
  tint(&j, "si_ind", r->si_ind); tint(&j, "short_msg_ind", r->short_msg_ind); tint(&j, "short_msg", r->short_msg);
  tint(&j, "tb_scaling", r->tb_scaling);
  tint(&j, "pdsch_abs_slot", nolayout ? -1 : r->pdsch_abs_slot);
  tint(&j, "harq_fb_abs_slot", nolayout ? -1 : r->harq_fb_abs_slot);
  tput(&j, ",\"decode\":%d}", (int)r->decode);
  return j.bad ? -1 : (int)j.w;
}

void nr_sense_emit_dci(nr_sense_dci_rec_t *r)
{
  if (!nr_sense_enabled() || !r) return;
  nr_sense_grant_meta_finish(&r->o, &r->m);
  if (!r->o.t_mono_ns) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); r->o.t_mono_ns = (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec; }
  nr_sense_push_dci(r);
}

void nr_sense_emit_decode(uint64_t abs_slot, uint16_t rnti, int dir, bool probe, int crc, float snr_db, const char *learned)
{
  if (!nr_sense_enabled()) return;
  char line[512], lr[128] = "";
  const bool has_l = learned != NULL;
  if (has_l) { size_t k = 0; for (; *learned && k < sizeof lr - 2; learned++) if (*learned != '"' && *learned != '\\' && (unsigned char)*learned >= 32) lr[k++] = *learned; lr[k] = 0; }
  char snr[32]; if (isfinite(snr_db)) snprintf(snr, sizeof snr, "%.7g", (double)snr_db); else strcpy(snr, "null");
  const int k = snprintf(line, sizeof line,
      "{\"schema\":2,\"type\":\"decode\",\"abs_slot\":%" PRIu64 ",\"rnti\":%u,\"dir\":\"%s\",\"probe\":%s,\"crc\":%d,\"snr_db\":%s,\"learned\":%s%s%s}",
      abs_slot, (unsigned)rnti, dir ? "UL" : "DL", probe ? "true" : "false", crc, snr,
      has_l ? "\"" : "null", lr, has_l ? "\"" : "");
  if (k > 0 && (size_t)k < sizeof line) nr_sense_push_record(line);
}
