#!/bin/bash
# Per-thread CPU of a running process over a window, from /proc/<pid>/task/*/stat (utime+stime).
# usage: thrprof.sh <pid> [secs=20] [name-regex=.]   -> lines "<thread-name> <cpu%>" (100 % = one core)
# Used by the Task A7 rfsim A/B to read passivePdcch0/1 while the receiver tracks.
set -u
PID=$1; SECS=${2:-20}; RE=${3:-.}
HZ=$(getconf CLK_TCK)
snap() {
  for t in /proc/"$PID"/task/*; do
    [ -r "$t/stat" ] || continue
    # comm may contain spaces: strip "pid (comm) " before splitting; fields 14,15 = utime,stime
    s=$(cat "$t/stat" 2>/dev/null) || continue
    name=$(cat "$t/comm" 2>/dev/null)
    rest=${s##*) }
    set -- $rest
    echo "${t##*/} $name $(( ${12} + ${13} ))"
  done
}
A=$(snap); sleep "$SECS"; B=$(snap)
join <(echo "$A" | sort -k1,1) <(echo "$B" | sort -k1,1) |
  awk -v hz="$HZ" -v s="$SECS" -v re="$RE" '$2 ~ re { printf "%s %.1f\n", $2, 100.0 * ($5 - $3) / hz / s }' |
  sort -k1,1
