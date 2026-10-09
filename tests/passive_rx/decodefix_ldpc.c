/* Recorded-input LDPC diagnostic. Input records: 12 uint32 parameters followed
 * by G int16 descrambled LLRs. Run only under heavy.sh. */
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "common/utils/LOG/log.h"
#include "common/utils/threadPool/thread-pool.h"
#include "PHY/CODING/nrLDPC_coding/nrLDPC_coding_interface.h"
#include "PHY/CODING/coding_defs.h"
#include "PHY/NR_TRANSPORT/nr_transport_common_proto.h"

int main(int argc, char **argv)
{
  if (argc != 3) return 2;
  logInit();
  crcTableInit();
  void *lib = dlopen(argv[1], RTLD_NOW | RTLD_GLOBAL);
  if (!lib) { fprintf(stderr, "%s\n", dlerror()); return 2; }
  nrLDPC_coding_decoder_t *decode = dlsym(lib, "nrLDPC_coding_decoder");
  if (!decode) return 2;
  FILE *f = fopen(argv[2], "rb");
  if (!f) return 2;
  tpool_t pool;
  char threads[] = "n";
  initTpool(threads, &pool, false);
  decode_abort_t ab;
  init_abort(&ab);
  uint32_t h[12];
  while (fread(h, sizeof(h), 1, f) == 1) {
    /* id, A, G, Qm, Nl, BG, RV, lbrm, iterations, scale numerator, denominator, unused */
    nrLDPC_TB_decoding_parameters_t tb = {0};
    tb.A = h[1]; tb.G = h[2]; tb.Qm = h[3]; tb.nb_layers = h[4]; tb.BG = h[5]; tb.rv_index = h[6];
    tb.tbslbrm = h[7]; tb.max_ldpc_iterations = h[8];
    nr_segmentation(NULL, NULL, lenWithCrc(1, tb.A), &tb.C, &tb.K, &tb.Z, &tb.F, tb.BG);
    tb.llr = malloc((size_t)tb.G * sizeof(int16_t));
    if (fread(tb.llr, sizeof(int16_t), tb.G, f) != tb.G) return 3;
    for (uint32_t i = 0; i < tb.G; i++) {
      int v = (int)tb.llr[i] * (int)h[9] / (int)h[10];
      tb.llr[i] = v < -32767 ? -32767 : v > 32767 ? 32767 : v;
    }
    uint32_t processed = 0;
    int llrlen = 0;
    tb.E = nr_get_E(tb.G, tb.C, tb.Qm, tb.nb_layers, 0);
    tb.E2 = tb.E; tb.first_rE2 = tb.C;
    tb.R = nr_get_R_ldpc_decoder(tb.rv_index, tb.E, tb.BG, tb.Z, &llrlen, 0);
    for (uint32_t r = 1; r < tb.C; r++) {
      int e = nr_get_E(tb.G, tb.C, tb.Qm, tb.nb_layers, r);
      if (e == tb.E) continue;
      tb.E2 = e; tb.first_rE2 = r;
      tb.R2 = nr_get_R_ldpc_decoder(tb.rv_index, e, tb.BG, tb.Z, &llrlen, 0);
      break;
    }
    tb.c = calloc(tb.C, tb.K / 8);
    tb.d = calloc((size_t)tb.C * 68 * tb.Z, sizeof(int16_t));
    tb.d_to_be_cleared = true; tb.processedSegments = &processed;
    tb.abort_decode = &ab; set_abort(&ab, false);
    nrLDPC_slot_decoding_parameters_t slot = {.nb_TBs=1, .threadPool=&pool, .TBs=&tb};
    int rc = decode(&slot);
    uint8_t *b = calloc(tb.C, tb.K / 8);
    int n = 0;
    for (uint32_t r = 0; r < tb.C; r++) {
      int bytes = tb.K / 8 - tb.F / 8 - (tb.C > 1 ? 3 : 0);
      memcpy(b + n, tb.c + r * (tb.K / 8), bytes); n += bytes;
    }
    int nonzero = 0;
    for (uint32_t i = 0; i < tb.A / 8; i++) nonzero |= b[i];
    int ok = rc == 0 && processed == tb.C && nonzero && check_crc(b, lenWithCrc(1,tb.A), crcType(1,tb.A));
    printf("OFFLINE id=%u crc=%d C=%u cb=", h[0], ok, tb.C);
    for (uint32_t r = 0; r < tb.C; r++) printf("%d", tb.decodeSuccess[r]);
    printf("\n");
    free(b); free(tb.c); free(tb.d); free(tb.llr);
  }
  fclose(f);
  return 0;
}
