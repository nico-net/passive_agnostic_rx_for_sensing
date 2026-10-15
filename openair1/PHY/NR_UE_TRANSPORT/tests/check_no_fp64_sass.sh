#!/bin/sh
# GPUPASS Task 5: the float instantiations of the demod kernels (k_tail<MM, float>, k_align<float>, k_mmse3_prep<float>) must contain no FP64 instruction in the SASS of
# libpdsch_gpu.so, for every architecture it carries (sm_89 and sm_121 in the build_t26 configuration). Usage: check_no_fp64_sass.sh [libpdsch_gpu.so] [arch ...]
# Exit 0 = clean, 1 = FP64 found or no float kernel found, 77 = no cuobjdump / library (skipped).
LIB=${1:-./libpdsch_gpu.so}
[ $# -gt 0 ] && shift
ARCHS=${*:-sm_89 sm_121}
command -v cuobjdump >/dev/null 2>&1 || { echo "no cuobjdump"; exit 77; }
[ -f "$LIB" ] || { echo "no $LIB"; exit 77; }
rc=0
for a in $ARCHS; do
  out=$(cuobjdump -sass -arch "$a" "$LIB" 2>/dev/null) || { echo "$a: no SASS in $LIB (arch not built)"; continue; }
  res=$(printf '%s\n' "$out" | awk '
    /^[ \t]*Function :/ { fn = $3; isf = (fn ~ /k_tailILi[0-9]+EfE|k_alignIfE|k_mmse3_prepIfE/); nf += isf; next }
    isf && /(^|[^A-Z0-9_])(DFMA|DADD|DMUL|DSETP|DSET|DMNMX|DFSEL)([^A-Z0-9_]|$)|\.F64|64H/ { bad++; if (bad <= 5) print "  " fn ": " $0 }
    END { printf "FUNCS %d BAD %d\n", nf, bad }')
  echo "$a: $(printf '%s\n' "$res" | tail -1)"
  printf '%s\n' "$res" | grep -v '^FUNCS' | head -5
  nf=$(printf '%s\n' "$res" | sed -n 's/^FUNCS \([0-9]*\) BAD.*/\1/p')
  bad=$(printf '%s\n' "$res" | sed -n 's/^FUNCS [0-9]* BAD \([0-9]*\)/\1/p')
  [ "${nf:-0}" -ge 8 ] || { echo "$a: expected 8 float kernels (k_tail 1,2,3,6,7,8, k_align, k_mmse3_prep), found ${nf:-0}"; rc=1; }
  [ "${bad:-0}" -eq 0 ] || rc=1
done
exit $rc
