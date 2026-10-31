/* Offline decode of EVERY code block of one ISAC_PDSCH_LLR_DUMP record, with the OFDM symbols each block covers (D6, diagnostic, not a test).
 * The receiver stops a TB after NR_CB_FAIL_LIMIT failed blocks, which hides where along the slot the failures start; this tool decodes all of them.
 * Usage: pdsch_cb_all_offline <dump> <record> <A=TBS bits> <rv> <tbslbrm (0 = no LBRM)> <nid (-1 = as dumped)> <llr_shift> <libldpc.so>
 *                             [BG (0 = both)] [dmrs symbols, e.g. 2,11 (slot symbol indices; default none)] [data REs per PRB on a DM-RS symbol (default 0)]
 * Output: one line per block "cb=<r> E=<E> bits=[a,b) sym=<first>..<last> iters=<n> ok=<0|1>", then a summary with the first failed block.
 * The LLR buffer is symbol-major (all layers of a symbol before the next symbol); a codeword bit b is in the symbol whose cumulative bit count
 * (12 REs per PRB, the DM-RS symbols' data REs as given, times Qm * Nl) contains it. A rate-matched slot (CSI-RS, PT-RS) changes the per-symbol
 * counts: the cumulative counts are scaled to G and a warning is printed when the nominal count differs. */
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "PHY/CODING/coding_defs.h"
#include "PHY/CODING/nrLDPC_defs.h"
#include "PHY/CODING/nrLDPC_coding/nrLDPC_coding_segment/nr_rate_matching.h"
#include "PHY/NR_TRANSPORT/nr_transport_common_proto.h"
#include "openair1/PHY/gold.h"
#include "common/config/config_userapi.h"
#include "common/utils/LOG/log.h"

configmodule_interface_t *uniqCfg = NULL;
int cpu_meas_enabled = 0; /* referenced by libldpc.so */
void exit_function(const char *file, const char *function, const int line, const char *s, const int assert)
{
  fprintf(stderr, "%s:%d %s() %s\n", file, line, function, s ? s : "");
  exit(1);
}

static void descramble(int16_t *l, uint32_t n, uint32_t c_init)
{
  uint32_t x1 = 0, x2 = c_init;
  uint32_t w = gold_generic(&x1, &x2, 1);
  for (uint32_t i = 0; i < n; i++) {
    if (i && (i & 31) == 0) w = gold_generic(&x1, &x2, 0);
    if ((w >> (i & 31)) & 1) l[i] = (int16_t)-l[i];
  }
}

/* sym_of_bit: the slot symbol holding codeword bit b, given the cumulative bit count at the end of each of the L symbols */
static int sym_of_bit(const uint64_t *cum, int L, uint64_t b)
{
  for (int i = 0; i < L; i++)
    if (b < cum[i]) return i;
  return L - 1;
}

int main(int argc, char **argv)
{
  if (argc < 9) { fprintf(stderr, "usage: %s dump rec A rv tbslbrm nid shift libldpc.so [BG] [dmrs syms] [dmrs data REs/PRB]\n", argv[0]); return 2; }
  logInit();
  set_glog(OAILOG_DISABLE);
  crcTableInit();
  const int rec = atoi(argv[2]);
  const uint32_t A = (uint32_t)atoi(argv[3]);
  const int rv = atoi(argv[4]);
  uint32_t tbslbrm = (uint32_t)atoi(argv[5]);
  const int nid = atoi(argv[6]), shift = atoi(argv[7]);
  const int bg_only = argc > 9 ? atoi(argv[9]) : 0;
  const int dmrs_res = argc > 11 ? atoi(argv[11]) : 0;
  int dmrs_sym[14], n_dmrs = 0;
  if (argc > 10)
    for (char *tok = strtok(argv[10], ","); tok && n_dmrs < 14; tok = strtok(NULL, ",")) dmrs_sym[n_dmrs++] = atoi(tok);
  FILE *f = fopen(argv[1], "rb");
  if (!f) { perror(argv[1]); return 2; }
  int32_t h[14];
  int16_t *llr = NULL;
  for (int k = 0;; k++) {
    char m[4];
    if (fread(m, 1, 4, f) != 4 || memcmp(m, "PDL1", 4) || fread(h, sizeof(h), 1, f) != 1) { fprintf(stderr, "no record\n"); return 2; }
    llr = realloc(llr, (size_t)h[12] * 2);
    if (fread(llr, 2, (size_t)h[12], f) != (size_t)h[12]) return 2;
    if (k == rec) break;
  }
  const uint16_t rnti = (uint16_t)h[0];
  const uint32_t G = (uint32_t)h[12];
  const int Qm = h[3], Nl = h[4], nrb = h[6], S = h[7], L = h[8];
  if (nid >= 0) {
    if (h[11]) descramble(llr, G, ((uint32_t)rnti << 15) + (uint32_t)h[10]);
    descramble(llr, G, ((uint32_t)rnti << 15) + (uint32_t)nid);
  } else if (!h[11]) {
    descramble(llr, G, ((uint32_t)rnti << 15) + (uint32_t)h[10]);
  }
  /* cumulative bits at the end of each symbol S..S+L-1, scaled to G */
  uint64_t cum[14], nominal = 0;
  if (L < 1 || L > 14) { fprintf(stderr, "bad L %d\n", L); return 2; }
  for (int i = 0; i < L; i++) {
    int res = 12;
    for (int d = 0; d < n_dmrs; d++)
      if (dmrs_sym[d] == S + i) res = dmrs_res;
    nominal += (uint64_t)res * (uint64_t)nrb * (uint64_t)Qm * (uint64_t)Nl;
    cum[i] = nominal;
  }
  if (nominal != G) {
    fprintf(stderr, "warning: nominal %llu bits != G %u (CSI-RS / PT-RS / other DM-RS?): symbol borders scaled to G\n", (unsigned long long)nominal, G);
    for (int i = 0; i < L; i++) cum[i] = nominal ? cum[i] * G / nominal : G;
  }
  void *lib = dlopen(argv[8], RTLD_LAZY);
  if (!lib) { fprintf(stderr, "%s\n", dlerror()); return 2; }
  LDPC_initfunc_t *ini = (LDPC_initfunc_t *)dlsym(lib, "LDPCinit");
  LDPC_decoderfunc_t *dec = (LDPC_decoderfunc_t *)dlsym(lib, "LDPCdecoder");
  if (!dec) { fprintf(stderr, "no LDPCdecoder\n"); return 2; }
  if (ini) ini();
  printf("rec=%d rnti=0x%04x G=%u Qm=%d Nl=%d A=%u PRB=%d S=%d L=%d\n", rec, rnti, G, Qm, Nl, A, nrb, S, L);
  for (int BG = 1; BG <= 2; BG++) {
    if (bg_only && BG != bg_only) continue;
    unsigned C = 0, K = 0, Z = 0, F = 0;
    nr_segmentation(NULL, NULL, (unsigned)lenWithCrc(1, A), &C, &K, &Z, &F, (uint8_t)BG);
    if (!C) continue;
    static int16_t e[300000], d[68 * 384 * 3 + 64], z[68 * 384 + 64];
    static int8_t l8[68 * 384 + 64];
    static uint8_t out[27008];
    uint64_t off = 0;
    int first_fail = -1, n_ok = 0;
    printf("BG%d C=%u K=%u Z=%u F=%u\n", BG, C, K, Z, F);
    for (unsigned r = 0; r < C; r++) {
      const int E = nr_get_E(G, (uint8_t)C, (uint8_t)Qm, (uint8_t)Nl, (uint8_t)r);
      if (E <= 0 || E > (int)(sizeof(e) / 2) || off + (uint64_t)E > G) { printf("cb=%u E=%d does not fit G\n", r, E); break; }
      nr_deinterleaving_ldpc((uint32_t)E, (uint8_t)Qm, e, llr + off);
      memset(d, 0, sizeof(d));
      const uint32_t lbrm = tbslbrm ? tbslbrm : 0x7fffffff;
      if (nr_rate_matching_ldpc_rx(lbrm, (uint8_t)BG, (uint16_t)Z, d, e, (uint8_t)C, (uint8_t)rv, 1, (uint32_t)E, F, K - F - 2 * Z) == -1) {
        printf("cb=%u rate matching failed\n", r);
        off += (uint64_t)E;
        continue;
      }
      const int Kc = BG == 2 ? 52 : 68;
      const int Kprime = (int)(K - F);
      memset(z, 0, 2 * Z * sizeof(*z));
      memset(z + Kprime, 127, F * sizeof(*z));
      memcpy(z + 2 * Z, d, (Kprime - 2 * Z) * sizeof(*z));
      memcpy(z + K, d + (K - 2 * Z), (Kc * Z - K) * sizeof(*z));
      for (int i = 0; i < Kc * (int)Z; i++) {
        int v = z[i] == 127 && i >= Kprime && i < (int)K ? 127 : (z[i] >> shift);
        l8[i] = (int8_t)(v > 127 ? 127 : v < -128 ? -128 : v);
      }
      int llrLen = 0;
      t_nrLDPC_dec_params dp = {.check_crc = check_crc};
      dp.BG = (uint8_t)BG;
      dp.Z = (uint16_t)Z;
      dp.numMaxIter = 20;
      dp.outMode = nrLDPC_outMode_BIT;
      dp.R = (uint8_t)nr_get_R_ldpc_decoder(rv, E, BG, (int)Z, &llrLen, 0);
      dp.crc_type = crcType((int)C, (int)A);
      dp.Kprime = lenWithCrc((int)C, (int)A);
      t_nrLDPC_time_stats ts;
      memset(&ts, 0, sizeof(ts));
      decode_abort_t ab;
      init_abort(&ab);
      set_abort(&ab, false);
      const int it = dec(&dp, l8, out, &ts, &ab);
      const int ok = it < dp.numMaxIter;
      const int s0 = sym_of_bit(cum, L, off), s1 = sym_of_bit(cum, L, off + (uint64_t)E - 1);
      printf("cb=%u E=%d bits=[%llu,%llu) sym=%d..%d iters=%d ok=%d\n", r, E, (unsigned long long)off, (unsigned long long)(off + (uint64_t)E), S + s0,
             S + s1, it, ok);
      if (ok) n_ok++;
      else if (first_fail < 0) first_fail = (int)r;
      off += (uint64_t)E;
    }
    printf("BG%d summary: %d of %u blocks pass, first failed block %d\n", BG, n_ok, C, first_fail);
  }
  return 0;
}
