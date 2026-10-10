/* Offline code-block-0 decode of one ISAC_PDSCH_LLR_DUMP record (T26 diagnostic, not a test).
 * Usage: pdsch_cb_offline <dump> <record> <A=TBS bits> <rv> <tbslbrm (0 = no LBRM)> <nid (-1 = as dumped)>
 *        <llr_shift> <libldpc.so>
 * Re-descrambles with c_init = rnti*2^15 + nid when nid >= 0, segments A+24 (TS 38.212 5.2.2), takes code block 0
 * (E per nr_get_E), de-interleaves, de-rate-matches and runs the CPU LDPC decoder with its CRC early stop.
 * Prints one line: ok=1 means the CB CRC (C > 1) or the TB CRC (C == 1) passed. */
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
/* exit_function: TODO(doc).
 *   How it works: TODO(doc).
 *   ISAC_* flags read: none.
 *   Callers: common/config/tests/test_config_cmdline.cpp:9, common/config/yaml/tests/test_yaml_config.cpp:14, common/platform_types.h:302, common/platform_types.h:306 (+94 more).
 */
void exit_function(const char *file, const char *function, const int line, const char *s, const int assert)
{
  fprintf(stderr, "%s:%d %s() %s\n", file, line, function, s ? s : "");
  exit(1);
}

/* descramble: TODO(doc).
 *   How it works: TODO(doc).
 *   ISAC_* flags read: none.
 *   Callers: openair1/PHY/NR_UE_TRANSPORT/tests/pdsch_cb_offline.c:63, openair1/PHY/NR_UE_TRANSPORT/tests/pdsch_cb_offline.c:64, openair1/PHY/NR_UE_TRANSPORT/tests/pdsch_cb_offline.c:66.
 */
static void descramble(int16_t *l, uint32_t n, uint32_t c_init)
{
  uint32_t x1 = 0, x2 = c_init;
  uint32_t w = gold_generic(&x1, &x2, 1);
  for (uint32_t i = 0; i < n; i++) {
    if (i && (i & 31) == 0) w = gold_generic(&x1, &x2, 0);
    if ((w >> (i & 31)) & 1) l[i] = (int16_t)-l[i];
  }
}

/* main: TODO(doc).
 *   How it works: TODO(doc).
 *   ISAC_* flags read: none.
 *   Callers: none (program entry point).
 */
int main(int argc, char **argv)
{
  if (argc < 9) { fprintf(stderr, "usage: %s dump rec A rv tbslbrm nid shift libldpc.so\n", argv[0]); return 2; }
  logInit();
  set_glog(OAILOG_DISABLE);
  crcTableInit(); /* without it every CRC computes to 0 and any zero-tailed word "passes" */
  const int rec = atoi(argv[2]);
  const uint32_t A = (uint32_t)atoi(argv[3]);
  const int rv = atoi(argv[4]);
  uint32_t tbslbrm = (uint32_t)atoi(argv[5]);
  const int nid = atoi(argv[6]), shift = atoi(argv[7]);
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
  const int Qm = h[3], Nl = h[4];
  if (nid >= 0) {
    if (h[11]) descramble(llr, G, ((uint32_t)rnti << 15) + (uint32_t)h[10]); /* undo the receiver's */
    descramble(llr, G, ((uint32_t)rnti << 15) + (uint32_t)nid);
  } else if (!h[11]) {
    descramble(llr, G, ((uint32_t)rnti << 15) + (uint32_t)h[10]);
  }
  void *lib = dlopen(argv[8], RTLD_LAZY);
  if (!lib) { fprintf(stderr, "%s\n", dlerror()); return 2; }
  LDPC_initfunc_t *ini = (LDPC_initfunc_t *)dlsym(lib, "LDPCinit");
  LDPC_decoderfunc_t *dec = (LDPC_decoderfunc_t *)dlsym(lib, "LDPCdecoder");
  if (!dec) { fprintf(stderr, "no LDPCdecoder\n"); return 2; }
  if (ini) ini();
  /* TS 38.212 6.2.2 base graph from the TBS and the code rate is unknown here: try both, report each. */
  for (int BG = 1; BG <= 2; BG++) {
    unsigned C = 0, K = 0, Z = 0, F = 0;
    nr_segmentation(NULL, NULL, (unsigned)lenWithCrc(1, A), &C, &K, &Z, &F, (uint8_t)BG);
    if (!C) continue;
    const int E = nr_get_E(G, (uint8_t)C, (uint8_t)Qm, (uint8_t)Nl, 0);
    static int16_t e[300000], d[68 * 384 * 3 + 64], z[68 * 384 + 64];
    static int8_t l8[68 * 384 + 64];
    static uint8_t out[27008];
    if (E <= 0 || E > (int)(sizeof(e) / 2)) continue;
    nr_deinterleaving_ldpc((uint32_t)E, (uint8_t)Qm, e, llr);
    memset(d, 0, sizeof(d));
    const uint32_t lbrm = tbslbrm ? tbslbrm : 0x7fffffff;
    if (nr_rate_matching_ldpc_rx(lbrm, (uint8_t)BG, (uint16_t)Z, d, e, (uint8_t)C, (uint8_t)rv, 1, (uint32_t)E, F,
                                 K - F - 2 * Z) == -1) { printf("BG%d rate matching failed\n", BG); continue; }
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
    int nz = 0;
    for (int i = 0; i < (Kprime >> 3); i++) nz += out[i] != 0;
    printf("rec=%d rnti=0x%04x G=%u Qm=%d Nl=%d A=%u BG=%d C=%u K=%u Z=%u F=%u E=%d R=%d iters=%d ok=%d nonzero_bytes=%d\n",
           rec, rnti, G, Qm, Nl, A, BG, C, K, Z, F, E, dp.R, it, it < dp.numMaxIter, nz);
    if (getenv("HEX") && it < dp.numMaxIter) {
      for (int i = 0; i < (Kprime >> 3); i++) printf("%02x", out[i]);
      printf("\n");
    }
  }
  return 0;
}
