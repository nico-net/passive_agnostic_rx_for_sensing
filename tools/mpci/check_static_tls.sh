#!/bin/sh
# Fails if the TLS segment of the given ELF binary has MemSiz >= 3 MiB (static-TLS budget, plan B.2).
# Usage: check_static_tls.sh <binary>   (run on the nr_cell test binary only; no TLS segment = pass)
set -eu
[ $# -eq 1 ] || { echo "usage: $0 <elf>" >&2; exit 2; }
line=$(readelf -lW "$1" | awk '$1=="TLS"{print}')
if [ -z "$line" ]; then echo "check_static_tls: $1 has no TLS segment: OK"; exit 0; fi
# columns: TLS Offset VirtAddr PhysAddr FileSiz MemSiz Flg Align
memsiz=$(echo "$line" | awk '{print $6}')
lim=$((3 * 1024 * 1024))
if [ $((memsiz)) -ge $lim ]; then echo "check_static_tls: FAIL TLS MemSiz $memsiz >= $lim" >&2; exit 1; fi
echo "check_static_tls: OK TLS MemSiz $((memsiz)) < $lim"
