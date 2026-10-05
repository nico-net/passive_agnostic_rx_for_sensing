# SP — sensing API CPU cost reduction

Host: DGX aarch64, 2026-10-05. Branch: `sense/perf` from `8208ed358c`. Evidence labels follow `PROJECT_MEMORY.md` §0.1.

## Result and changes

- [MEASURED, DGX aarch64 rfsim 106 PRB 4 RX, 7db8ac6883; supplied by operator] The motivating 420 s arms consumed 832/847 s receiver CPU with the API off and 1300/1291 s with all nine configured tokens, four CFR antennas and local NVMe sinks: roughly **+55%**. The all-source arm had zero export drops, about 17 MB/s CFR, and about 38.8k frames/run: SSB 9.5k, CSI-RS 0.8k, PDSCH data 14.4k and PDSCH DM-RS 14.0k. This branch has no post-change live CPU measurement. The ≤ +10% target remains an operator gate.
- [CODE-READ, DGX aarch64] `nr_sense_api.{h,c}` adds per-source production and packing nanoseconds/counts, total record formatting and writer syscall time/counts, all cumulative and only clocked in enabled sensing paths. The exporter already reserved ring slots and packed directly into them; this change keeps that path, uses `memcpy` for native little-endian `u16` and float IQ arrays, and combines wide-index validation with packing. The writer sends up to 16 adjacent slots per `writev` and alternates bounded runs of eight when both streams have work. Short writes, EINTR, test chunking, ZMQ per-item messages, and drop-on-full remain supported.
- [CODE-READ, DGX aarch64] `nr_sense_records.{h,c}` queues copied RS metadata and bounded copies of its strings, then formats JSONL on the writer thread. Session and grant records still use their existing producer formatter and are included in `sense_rec_us`. Long RS strings use the original producer formatter. The writer formats outside the ring mutex. Record and CFR wire formats are unchanged.
- [CODE-READ, DGX aarch64] `nr_passive_metrics.{h,c}` and `nr_passive_metrics_json.c` publish `sense_prod_us_<src>`, `sense_pack_us_<src>`, `sense_prod_count_<src>`, `sense_pack_count_<src>` for `ssb`, `csirs`, `pdsch_dmrs`, `pdsch_data`, `pusch_dmrs`, `pusch_data`, `prach`, `srs`, plus `sense_rec_us`, `sense_write_us`, `sense_rec_count`, `sense_write_count`. The metrics buffer is now 16 KiB for the larger JSON object.
- [CODE-READ, DGX aarch64] Timers cover SSB export conversion (`phy_procedures_nr_ue.c`), CSI-RS LS export conversion (`csi_rx.c`), PDSCH data re-encode/reconstruction (`nr_pdsch_data_aided.c`), PDSCH DM-RS extraction and monitor channel estimation when the receiver SNR gate does not also require it (`nr_pdcch_blind_monitor_rt.c`, `nr_sense_gating.h`), PUSCH DM-RS extraction (`nr_pusch_passive_decode.c`), PUSCH data reconstruction (`nr_pusch_data_aided.c`), PRACH detection (`nr_prach_passive.c`), and SRS worker processing (`nr_srs_passive.c`). No receiver decoding algorithm changed.

## Interpreting the counters

- [CODE-READ, DGX aarch64] Use differences between two `ISAC_METRICS` lines from the same receiver run. `sense_prod_us_*` is enabled-source work before export; `sense_pack_us_*` is frame-slot packing. `sense_prod_count_*` counts measured producer invocations, while `sense_pack_count_*` counts accepted packed frames. `sense_rec_count` counts formatter calls, and `sense_write_count` counts writer batches (short-write retries can make more than one syscall). All `_us` values are cumulative integer microseconds rounded down from nanoseconds.
- [CODE-READ, DGX aarch64] Production/packing/record/write timers use monotonic elapsed time. The totals can overlap across threads and may include waiting in the LDPC encoder or filesystem; they are **diagnostic attribution**, not a substitute for receiver user+system CPU. `sense_write_us` covers file writes, not ZMQ sends. `pdsch_dmrs_blind` and `pdsch_dmrs` share the `pdsch_dmrs` wire counter. The monitor estimate is charged to sensing whenever DM-RS export requires it and the receiver SNR gate is off; enabling decoding does not make that estimate shared, because the decoder makes its own.
- [CODE-READ, DGX aarch64] `pdsch_data` fundamentally requires reconstructing the decoded TB (segmentation, LDPC encode, scrambling, modulation, RE mapping, then Y/X for every exported antenna/RE). `pdsch_dmrs` fundamentally needs a monitor channel estimate on accepted blind grants, including grants which the later decoder does not decode. Moving export to that decoder would change the exported frame set. SSB and CSI-RS reuse receiver estimates but must convert and copy their REs. UL PUSCH data has the same data-aided reconstruction cost; PUSCH DM-RS converts an existing estimate and corrects the FFT advance. SRS performs blind search plus LS estimation; PRACH performs occasion detection. Restricting the default source set may be required to meet +10%; no unmeasured claim is made that export changes alone meet it.
- [CODE-READ, DGX aarch64] The deferred PDSCH path reconstructs once after a successful decode; the inline path is the alternative path, not a second reconstruction of that queued grant. Shared decoding estimates remain intact. Production changes are instrumentation only: no duplicate reconstruction or safely removable estimator work was established in this pass. The removed duplicate traversal was export index validation, and native array copying replaces export element serialization.

## Verification

- [OFFLINE VERIFIED, DGX aarch64] Final ON build: `flock -s -w 7200 /tmp/td_measure.lock nice -n 19 env CCACHE_DISABLE=1 ninja -C cmake_targets/ran_build/build -j8 nr-uesoftmodem oai_usrpdevif rfsimulator params_libconfig nr-softmodem tests` — PASS. Final OFF build: same command with `build_off` and targets `nr-uesoftmodem tests` — PASS. Logs: `/tmp/sense_sp_build_final_build.log`, `/tmp/sense_sp_build_off_final_build.log`.
- [OFFLINE VERIFIED, DGX aarch64] ON and OFF focused CTest: `flock -s -w 7200 /tmp/td_measure.lock nice -n 19 ctest --test-dir cmake_targets/ran_build/<mode> -j4 -R '^(test_nr_prach_passive|test_nr_srs_blind|test_nr_sense_api|test_nr_sense_records|test_nr_passive_metrics|test_nr_isac_adapter|test_nr_sense_gating|test_nr_isac_ssb_source)$' --output-on-failure` — **8/8 in each mode**. An earlier PRACH run caught the fixture's wrapped record sink; the fallback path was fixed before these final runs. New cases check counter reset/snapshot, batched CFR byte layout, copied RS strings and exact JSON bytes, and the new metric keys.
- [OFFLINE VERIFIED, DGX aarch64] Shuffled seeds 1/3/5 for all eight focused binaries plus `test_nr_pdcch_blind_monitor` in both modes: **54/54 invocations passed** on the final code. Each used `flock -s -w 7200 /tmp/td_measure.lock nice -n 19 cmake_targets/ran_build/<mode>/<binary> --gtest_shuffle --gtest_random_seed=<seed> --gtest_brief=1`. Log: `/tmp/sense_sp_final_shuffle.log`.
- [OFFLINE VERIFIED, DGX aarch64] `flock -s -w 7200 /tmp/td_measure.lock nice -n 19 python3 -m unittest discover -s tools/sense_api -p 'test_*.py' -v` — **4/4 passed**, including NumPy output. Log: `/tmp/sense_sp_reader.log`. The C `SenseApi.GoldenReaderFixture` also passed under the shared lock; complete CFR bytes matched the canonical little-endian `struct.pack('<4sHHQBBHIQffII', b'NRCF', 1, 48, 1, 1, 2, 0, 2, 42, .25, .5, 3, 4)` header, index/padding arrays and eight floats, and JSONL matched all three golden lines exactly. The deferred RS test compares complete formatted JSON with the pure formatter while overwriting the producer's original string buffer.
- [OFFLINE VERIFIED, DGX aarch64 sandbox] Full ON CTest except the long simulation, rerun after the final monitor-chest attribution correction: `flock -s -w 7200 /tmp/td_measure.lock nice -n 19 ctest --test-dir cmake_targets/ran_build/build -j4 -E '^nr_td_sim_test$' --output-on-failure` — **144/152 passed**, 236.79 s; eight documented baseline/sandbox failures: `test_nr_pusch_ra0_qam64`, `dft_test`, `test_nr_modulation`, `time_management_tests`, `test_gtp`, `test_vrtsim`, `test_vrtsim_cirdb`, `nr_cuup_functional_test`. The known intermittent `test_nr_pusch_ra0_qam256` passed this final run. Log: `/tmp/sense_sp_final_ctest_fast.log`.
- [OFFLINE VERIFIED, DGX aarch64] Long simulation registration: `flock -s -w 7200 /tmp/td_measure.lock nice -n 19 ctest --test-dir cmake_targets/ran_build/build -j4 -R '^nr_td_sim_test$' --output-on-failure` — **1/1 passed**, 1600.24 s; log `/tmp/sense_sp_final_td_sim.log`. Across these two CTest commands, all 153 registrations are covered: **145 passed, eight known failures**.

[OFFLINE VERIFIED, DGX aarch64] Every build and test checked that `/home/nicola/NICOLA/wt/rr-orchestration/BUSY` was absent and took a separate shared lock. No rfsim, OTA, GPU benchmark, or exclusive lock was run here.

## DEFERRED MEASUREMENTS — orchestrator

[NOT RUN, DGX aarch64] Run these six 420 s 4-RX arms on the idle host, each under its own exclusive lock. `/tmp` is on the DGX local NVMe (`/dev/nvme0n1p2`, ext4). Keep `ISAC_RX_BRANCH_FO` and `ISAC_RECONF` unset and leave the merged-main acceleration defaults in force. Run the commands from this worktree after its commit is integrated; use distinct output paths so no run overwrites another. Repeat the six-arm block on another idle slot if run-to-run noise exceeds the margin to +10%.

```sh
sp_arm() {
  name=$1; sources=$2
  if [ "$sources" = off ]; then
    extra='--ue-nb-ant-rx 4 --sensing.enable 0'
  else
    extra="--ue-nb-ant-rx 4 --sensing.enable 1 --sensing.sources $sources --sensing.records_path /tmp/sense_sp_${name}.jsonl --sensing.cfr_path /tmp/sense_sp_${name}.cfr --sensing.cfr_antennas 4 --sensing.cfr_ring_frames 32 --sensing.cfr_max_re 46200 --sensing.rec_ring_slots 1024"
  fi
  flock -x -w 7200 /tmp/td_measure.lock env -u ISAC_RX_BRANCH_FO -u ISAC_RECONF OUT="/tmp/sense_sp_${name}" GATE_SECS=420 RXEXTRA="$extra" tests/passive_rx/dgx/rfsim_regress.sh 1
}
sp_arm api_off off
sp_arm api_all 'csi_rs,pdsch_dmrs,pdsch_dmrs_blind,pdsch_data,pusch_dmrs,pusch_data,ssb,prach,srs'
sp_arm pdsch_data 'pdsch_data'
sp_arm pdsch_dmrs 'pdsch_dmrs'
sp_arm ssb 'ssb'
sp_arm csirs 'csi_rs'
```

For each arm, inspect `base_r1/rx/time.txt` under its `OUT` directory and add `User time (seconds)` + `System time (seconds)` for receiver CPU. Compare each enabled arm with `api_off` using `(arm_cpu/off_cpu - 1) * 100`; require gate PASS, zero new CFR/record drops, and `api_all` ≤ +10% to retain all sources as a default. Extract the first and last `SENSING: ISAC_METRICS` JSON from `base_r1/rx/rx.log` and subtract the counters described above. A source-only CPU delta reveals its incremental receiver cost in that bed; the deltas need not add because receiver work and source gates share computation. Check JSONL/CFR links with `python3 tools/sense_api/read_sense.py /tmp/sense_sp_<name>.jsonl /tmp/sense_sp_<name>.cfr` for every enabled arm. Use the source-only deltas and record/frame counts to choose a source set whose combined arm meets +10%, then confirm that chosen set in a new 420 s gate arm.

[NOT RUN, DGX aarch64] After those arms, this prints all new counter deltas, dropping logging prefixes and colour escapes around the JSON:

```sh
python3 - <<'PY'
import json
from pathlib import Path
for arm in ('api_off', 'api_all', 'pdsch_data', 'pdsch_dmrs', 'ssb', 'csirs'):
    samples = []
    for line in Path(f'/tmp/sense_sp_{arm}/base_r1/rx/rx.log').read_text().splitlines():
        if 'ISAC_METRICS ' in line:
            raw = line.split('ISAC_METRICS ', 1)[1]
            samples.append(json.loads(raw[raw.index('{'):raw.rindex('}')+1]))
    first, last = samples[0], samples[-1]
    prefixes = ('sense_prod_', 'sense_pack_', 'sense_rec_', 'sense_write_')
    print(arm, {k: last[k] - first[k] for k in last if k.startswith(prefixes)})
PY
```

## Open limits

[IMPLEMENTED, NOT LIVE VALIDATED] The requested all-source +10% CPU target and post-change source ranking await the exclusive-lock arms above. The DGX rfsim bed has no PUSCH traffic, so UL source production counters will stay zero there. Existing rank > 1 CB0, K45 scan-queue drops, and the convergence defaults are unchanged.
