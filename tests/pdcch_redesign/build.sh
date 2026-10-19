#!/bin/bash
# Standalone build of the PDCCH-REDESIGN micro-benchmarks (g++/gcc/nvcc, no cmake). Usage: ./build.sh [outdir]
set -e
R=$(cd "$(dirname "$0")/../.." && pwd); O=${1:-/tmp/pdcch_redesign_bin}; mkdir -p "$O/obj"
INC="-I$R -I$R/openair1 -I$R/common -I$R/common/utils -I$R/common/utils/LOG -I$R/common/config -I$R/openair2 -I$R/nfapi/open-nFAPI/nfapi/public_inc"
S=$R/openair1/PHY/CODING/nrPolar_tools
for f in nr_polar_init nr_bitwise_operations nr_crc_byte nr_polar_decoder nr_polar_decoding_tools nr_polar_encoder nr_polar_interleaving_pattern nr_polar_kernal_operation nr_polar_kronecker_power_matrices nr_polar_matrix_and_array nr_polar_procedures nr_polar_sequence_pattern; do
  gcc -O3 -mcpu=native -w -c $INC $S/$f.c -o $O/obj/$f.o
done
gcc -O3 -mcpu=native -w -c $INC $R/openair1/PHY/CODING/crc_byte.c -o $O/obj/crc_byte2.o
gcc -O3 -mcpu=native -w -c $INC $R/common/utils/bits.c -o $O/obj/bits.o
g++ -O3 -mcpu=native -w $INC $R/tests/pdcch_redesign/polar_bench.cc $O/obj/*.o -lpthread -lm -o $O/polar_bench
gcc -O3 -mcpu=native -w -c $INC $R/openair1/PHY/TOOLS/oai_dfts_neon.c -o $O/obj_dft.o
g++ -O2 -mcpu=native -w $R/tests/pdcch_redesign/handoff_bench.cc $O/obj_dft.o -lpthread -lm -o $O/handoff_bench
echo built in $O
