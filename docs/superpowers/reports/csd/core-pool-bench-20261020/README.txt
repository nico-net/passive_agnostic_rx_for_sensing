[Bench, cloud x86] 4 cpus, no GPU. raw/<pool|cfs>_N<n>_s<scale>_r<rep>.txt = one 15 s run of
  taskset -c 0-3 bench_nr_core_pool 0 <n> <scale> 15 [cfs]   (build of common/utils/tests/bench_nr_core_pool.cpp at the working tree after 18a4153)
summary_table.txt / summary_medians.csv = medians over 3 repeats. wait = arrival->job start, lat = arrival->done (us). token wait columns = CORE_POOL histogram
bin upper bound (us) / max. miss = lat above deadline (scan 500 us, DL/UL 2 ms) + jobs undone at end.
