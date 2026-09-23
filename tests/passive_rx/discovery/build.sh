#!/bin/bash
# Build idsweep_offline_gpu (blind PDCCH DM-RS nID sweep, stage 1 + 2, for discover_live.sh):
# CUDA 12.4, sm_89, g++-13 host compiler for nvcc; the C side as the original captures/ build.
set -e
cd "$(dirname "$0")"
R=$(cd ../../.. && pwd)
nvcc -O3 -arch=sm_89 -ccbin g++-13 -c idsweep_gpu.cu -o idsweep_gpu.o
gcc -O3 -march=native -fopenmp -DUSE_GPU -I"$R" -c idsweep_offline.c -o idsweep_offline_gpu.o
nvcc -ccbin g++-13 -Xcompiler -fopenmp idsweep_offline_gpu.o idsweep_gpu.o -o idsweep_offline_gpu -lm
