#!/bin/sh
# GPUPASS Task 5: the float instantiations of the kernels an FP32 pass class launches (demod: k_tail<MM,float>, k_align<float>, k_mmse3_prep<float>; EVM chain:
# k_evm1/2/3, k_evm_fold, k_evm_out with float storage; chest post k_chest_post<float>) must contain no FP64 instruction in the SASS of libpdsch_gpu.so, for every
# architecture it carries (default: the cubins found in the library; pass arches to check specific ones). A kernel is "float" when its demangled template arguments name no double.
# Allowed: F2F.F64.F32 in k_evm_out<float> (it exports the float sums into the host's double nr_gpu_evm_sym_t; one thread per symbol).
# Informational (never fails): other kernels with FP64 (k_level's one-thread decision, the FEP phase of k_fep_gather / k_fep_rotate: a few ops per sample, kept
# double for bit-exact phase).
# Usage: check_no_fp64_sass.sh [libpdsch_gpu.so] [arch ...]. Exit 0 = clean, 1 = FP64 found / no float kernel / the arches disagree, 77 = no cuobjdump / library.
LIB=${1:-./libpdsch_gpu.so}
[ $# -gt 0 ] && shift
ARCHS=$*
command -v cuobjdump >/dev/null 2>&1 || { echo "no cuobjdump"; exit 77; }
command -v c++filt >/dev/null 2>&1 || { echo "no c++filt"; exit 77; }
[ -f "$LIB" ] || { echo "no $LIB"; exit 77; }
# default: the architectures the library actually carries as cubins (a build may drop unsupported ones, e.g. sm_121 below CUDA 12.9)
[ -n "$ARCHS" ] || ARCHS=$(cuobjdump -lelf "$LIB" 2>/dev/null | sed -n 's/.*\.\(sm_[0-9a-z]*\)\.cubin.*/\1/p' | sort -u)
[ -n "$ARCHS" ] || { echo "no cubin in $LIB"; exit 77; }
rc=0
nf0=
for a in $ARCHS; do
  out=$(cuobjdump -sass -arch "$a" "$LIB" 2>/dev/null | c++filt) || { echo "$a: no SASS in $LIB (arch not built)"; continue; }
  res=$(printf '%s\n' "$out" | awk '
    /^[ \t]*Function :/ {
      fn = $0; sub(/^[ \t]*Function : /, "", fn)
      ta = fn; sub(/>\(.*/, "", ta)
      isf = (fn ~ /(k_tail|k_align|k_mmse3_prep|k_evm1|k_evm2|k_evm3|k_evm_fold|k_evm_out|k_chest_post)</ && ta !~ /double/)
      isout = (fn ~ /k_evm_out</)
      nf += isf; if (isf) names[nf] = fn
      cur = fn; next }
    /(^|[^A-Z0-9_])(DFMA|DADD|DMUL|DSETP|DSET|DMNMX|DFSEL)([^A-Z0-9_]|$)|\.F64|64H/ {
      if (isf) { if (isout && $0 ~ /F2F\.F64\.F32/) next; bad++; if (bad <= 6) print "  " cur ": " $2 " " $3 " " $4 }
      else other[cur]++ }
    END { for (f in other) if (f !~ /double/) print "  info (FP64 by design): " substr(f, 1, 60) " x" other[f]; printf "FUNCS %d BAD %d\n", nf, bad }')
  printf '%s\n' "$res" | grep -v '^FUNCS'
  echo "$a: $(printf '%s\n' "$res" | tail -1)"
  nf=$(printf '%s\n' "$res" | sed -n 's/^FUNCS \([0-9]*\) BAD.*/\1/p')
  bad=$(printf '%s\n' "$res" | sed -n 's/^FUNCS [0-9]* BAD \([0-9]*\)/\1/p')
  [ "${nf:-0}" -ge 1 ] || { echo "$a: no float kernel found"; rc=1; }
  [ -z "$nf0" ] && nf0=$nf
  [ "$nf" = "$nf0" ] || { echo "$a: $nf float kernels, the first arch had $nf0"; rc=1; }
  [ "${bad:-0}" -eq 0 ] || rc=1
done
exit $rc
