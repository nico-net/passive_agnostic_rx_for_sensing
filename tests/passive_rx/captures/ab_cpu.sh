#!/bin/bash
# A/B on NIC-vs-softmodem CPU SEPARATION.
#
# HYPOTHESIS (measured 2026-09-02, see nic.csv work): UHD recv ERROR_CODE_TIMEOUT and NIC
# rx_out_of_buffer are ONE event, they PRECEDE the RFSTALL, and they are host-side -- packets
# arrived and were binned for want of a ring slot while recv() returned zero, which means NAPI
# was not scheduled. Not throughput (box is ~75 % idle, rings at the 8192 max, rmem_max 250 MB):
# LATENCY. All 14 mlx5 completion IRQs sit one-per-core across 0-13, i.e. on every core the
# softmodem's threads use, including cpu0 where UEthread_0 (the thread that drains UHD) runs.
#
# ARMS: "off" = stock, IRQ i -> core i, no taskset.
#       "on"  = NIC IRQs confined to 8-13, softmodem confined to 0-7 (which includes the
#               isolcpus/nohz_full island at 2-3 that is currently 100 % idle). No overlap.
#
# PRIMARY METRIC is ERROR_CODE_TIMEOUT count per run -- direct, high-count, and upstream of
# everything else. Run length and CPI yield are secondary: they were too noisy to settle the
# ISAC_TSYNC_RESET A/B and there is a second, non-NIC failure path still unaccounted for.
#
# Arms ALTERNATE rep-by-rep: this rig drifts over tens of minutes and blocked arms would
# attribute that drift to the knob.
set -u
cd /home/sens/NICOLA/captures || exit 1
NIC=enp129s0f0np0
COMP_IRQS="152 155 156 157 158 159 160 161 162 163 164 165 166 167"   # mlx5_comp0-13 @ 0000:81:00.0
REPS=${REPS:-5}
DUR=${DUR:-300}
rm -f /tmp/ab_cpu.stop

set_irqs() {   # $1 = "identity" | space-separated core list to round-robin over
  local i n=0 cores=($2)
  for i in $COMP_IRQS; do
    if [ "$1" = identity ]; then echo $n | sudo tee /proc/irq/$i/smp_affinity_list >/dev/null
    else echo "${cores[$((n % ${#cores[@]}))]}" | sudo tee /proc/irq/$i/smp_affinity_list >/dev/null; fi
    n=$((n+1))
  done
}

trap 'echo "restoring stock IRQ affinity"; set_irqs identity ""' EXIT

for r in $(seq 1 "$REPS"); do
  for arm in off on; do
    [ -e /tmp/ab_cpu.stop ] && { echo "STOPPED"; exit 0; }
    if [ "$arm" = on ]; then set_irqs list "8 9 10 11 12 13"; CS=0-7; else set_irqs identity ""; CS=; fi
    echo "=== rep $r arm=$arm $(date +%H:%M:%S) irq=$(cat /proc/irq/152/smp_affinity_list) cpuset=${CS:-none} ==="
    env ARM="cpu${arm}" CONF=/home/sens/NICOLA/nrue.passive_rx.ul.q.conf \
        DUR="$DUR" TRIES=1 RXG=20 NANT=4 MRC=3 SENSECOMB=1 SYNCONLY=1 CONTFO=1 \
        SLOTPOOL=512 GAINTRIM="4.5,11.6,0,6.1" ULPROBE=1 ${CS:+CPUSET=$CS} \
        bash ./run_arm.sh 2>&1 | grep -aE 'verdict=|PREFLIGHT'
    sleep 3
  done
done
echo "=== AB DONE ==="
