# P02 fixture audit — 2026-09-11

Tree `/home/sens/NICOLA/adaptive-rx-sensing`, branch `merge/adaptive-sensing`, commit
`e4c8cadd5f22b40605f82f6adedc838ee8e056f5` at generation time. Current build
`cmake_targets/ran_build/build/nr-uesoftmodem` sha256
`b064c9957bc1b68d717543eb2bac28c4b2616115bc675f5fed38685f008742fe` (matches P01's
`tests/passive_rx/baselines/manifest_20260910_6fcb7a6-dirty.json` `build.nr_uesoftmodem.sha256`
field — confirmed identical, binary unrebuilt since P01 generated that manifest even though the
manifest's own `git.commit`/`git.tracked_diff_patch_sha256` fields are stale relative to the live
tree; see "P01 manifest staleness" note at the end).

## 1. Tool

`tests/passive_rx/replay_header.py` (new, stdlib only) reads `replay_header_t`
(`openair1/PHY/NR_UE_TRANSPORT/nr_passive_replay_capture.c:28-38`) without hardcoding the
`NR_DL_FRAME_PARMS`-dependent offset of the `fp` field: it derives `fp`'s offset from the file's
own `header_bytes`/`fp_bytes`/`job_bytes` fields (`header_bytes - fp_bytes - REPLAY_SLOTS*16 -
REPLAY_UL*24 - REPLAY_DL*(job_bytes+16)`), so it is exact for whatever struct-alignment padding a
given build inserted, not a guess. `REPLAY_SLOTS`/`REPLAY_UL`/`REPLAY_DL` (2560/2048/512) and the
fixed `replay_slot_t`/`replay_ul_t` sizes (16/24 bytes) are cited from
`nr_passive_replay_capture.c:19-32`. The layout (including the `replay_dl_t` = `job_bytes+16`
padding, and a 12-byte hole before `fp` on this build's ABI) was independently verified against the
live build's own DWARF debug info before trusting the hand-derivation:

```
gdb -q -batch -ex 'set max-value-size unlimited' -ex 'ptype /o replay_header_t' \
    cmake_targets/ran_build/build/CMakeFiles/PHY_NR_UE.dir/openair1/PHY/NR_UE_TRANSPORT/nr_passive_replay_capture.c.o
```
confirmed `fp` at offset 64 (12-byte hole after `n_dl`), `sizeof(replay_slot_t)=16`,
`sizeof(replay_ul_t)=24`, `sizeof(replay_dl_t)=400` with `job_bytes=384` (`tb_hash` at
`job_bytes`, `tb_bytes` at `job_bytes+8`, 4-byte trailing pad) — all consistent with
`replay_header.py`'s formula-derived offsets, which do not hardcode the 64/12 numbers themselves.
Every one of the 22 runs below independently confirmed `file_bytes == header_bytes + iq_bytes` and
`magic` match, which is a second, per-file cross-check that the derived offsets are self-consistent
(a wrong `dl_offset` would misalign every `tb_bytes` read, but would not by itself break the
unrelated `file_bytes` total — both checks passing on all 22 files is evidence the tool is reading
records at the correct offset, not coincidentally computing a plausible-looking wrong one).

`replay.bin` is `root:600`; reading it requires `sudo -n python3 replay_header.py <file>` (verified
`sudo -n true` succeeds passwordless on this host).

## 2. Pre-existing fixtures (`/home/sens/NICOLA/captures/*/replay.bin`)

22 found. All 22 are **INADMISSIBLE** on two independent grounds: producing binary does not match
the current build, AND `receiver.conf` carries `pdcch_blind_monitor_full_auto = 1` (VOID under the
plan's TESTING MODE RULE, §1 2026-09-11 amendment — these captures all predate that rule). First
failing reason recorded per the brief is the binary mismatch (checked first in the admissibility
order "same binary AND full_auto=0 AND ...").

| capture | replay.bin bytes | source_commit.txt | binary matches current | full_auto | header version | n_ul | n_dl records | dl success/failure (tb_bytes>0 / ==0) | verdict | first failing reason |
|---|---:|---|---|---:|---:|---:|---:|---|---|---|
| adaptive_ul_dl_mrc2.1lxMai | 316268528 | aa7f82acedf35e21925c84e4a19e79c2e320f1e3 | NO | 1 | 1 | 32 | 1 | 1/0 | INADMISSIBLE | binary mismatch |
| adaptive_ul_dl_mrc2.3aBLA3 | 316268528 | dc859c8fa66b3bc59b4ec30cf8afdcb1ec6941ee | NO | 1 | 2 | 21 | 56 | 20/36 | INADMISSIBLE | binary mismatch |
| adaptive_ul_dl_mrc2.Av693P | 316268528 | dc859c8fa66b3bc59b4ec30cf8afdcb1ec6941ee | NO | 1 | 1 | 18 | 1 | 1/0 | INADMISSIBLE | binary mismatch |
| adaptive_ul_dl_mrc2.HqNljS | 316268528 | dc859c8fa66b3bc59b4ec30cf8afdcb1ec6941ee | NO | 1 | 1 | 28 | 1 | 1/0 | INADMISSIBLE | binary mismatch |
| adaptive_ul_dl_mrc2.LYKfRD | 316268528 | 30fdbd69e8eeb2517e3160905fdebb14c806cfde | NO | 1 | 1 | 24 | 1 | 1/0 | INADMISSIBLE | binary mismatch |
| adaptive_ul_dl_mrc2.NoxMgb | 316268528 | dc859c8fa66b3bc59b4ec30cf8afdcb1ec6941ee | NO | 1 | 1 | 25 | 2 | 2/0 | INADMISSIBLE | binary mismatch |
| adaptive_ul_dl_mrc2.QACZSc | 316268528 | 1fd296e9b4fdc51115a085588bc3327be614c657 | NO | 1 | 1 | 16 | 1 | 1/0 | INADMISSIBLE | binary mismatch |
| adaptive_ul_dl_mrc2.SFFMM9 | 316268528 | 51ecd85c1be7bcb5a42cba2dc4252dcdc2cf9baf | NO | 1 | 2 | 23 | 141 | 125/16 | INADMISSIBLE | binary mismatch |
| adaptive_ul_dl_mrc2.XJn8TI | 316268528 | dc859c8fa66b3bc59b4ec30cf8afdcb1ec6941ee | NO | 1 | 1 | 32 | 1 | 1/0 | INADMISSIBLE | binary mismatch |
| adaptive_ul_dl_mrc2.dKSWou | 316268528 | 9a7816458c1e5d221adc460f1b60c429464be50a | NO | 1 | 1 | 18 | 1 | 1/0 | INADMISSIBLE | binary mismatch |
| adaptive_ul_dl_mrc2.full.gH58Un | 316268528 | (none) | NO | 1 | 2 | 20 | 86 | 22/64 | INADMISSIBLE | binary mismatch |
| adaptive_ul_dl_mrc2.noGySh | 316268528 | 4fff09b4045bb9c63af37acb0f453751c81eb5ee | NO | 1 | 1 | 32 | 1 | 1/0 | INADMISSIBLE | binary mismatch |
| adaptive_ul_dl_mrc2.tTftbV | 316268528 | 9a7816458c1e5d221adc460f1b60c429464be50a | NO | 1 | 1 | 32 | 1 | 1/0 | INADMISSIBLE | binary mismatch |
| adaptive_ul_dl_mrc2.wFHFMQ | 316268528 | 03af4c581dec37e3e33a7e0f972dc9e0116c9086 | NO | 1 | 1 | 19 | 1 | 1/0 | INADMISSIBLE | binary mismatch |
| adaptive_ul_dl_mrc2.za4Dat | 316268528 | 21626c0108d103d66f3b3eeec47789b8157fa3b9 | NO | 1 | 1 | 24 | 1 | 1/0 | INADMISSIBLE | binary mismatch |
| auto_all_cfr.3yNhzk | 316268528 | (none) | NO | 1 | 2 | 3 | 20 | 20/0 | INADMISSIBLE | binary mismatch |
| auto_crc_pinned.3ufNcO | 316268528 | (none) | NO | 1 | 2 | 16 | 37 | 34/3 | INADMISSIBLE | binary mismatch |
| auto_ul_baseline_fix.SjPICx | 316268528 | (none) | NO | 1 | 2 | 14 | 33 | 30/3 | INADMISSIBLE | binary mismatch |
| auto_ul_evidence_retry.G8BYYc | 316268528 | (none) | NO | 1 | 2 | 15 | 51 | 45/6 | INADMISSIBLE | binary mismatch |
| auto_ul_post_reboot.hgledw | 316268528 | (none) | NO | 1 | 2 | 11 | 40 | 38/2 | INADMISSIBLE | binary mismatch |
| auto_ul_rf_restore_retry.cwCFwg | 316268528 | (none) | NO | 1 | 2 | 19 | 37 | 33/4 | INADMISSIBLE | binary mismatch |
| auto_ul_sched_probe.zfDNuN | 316268528 | (none) | NO | 1 | 2 | 13 | 43 | 36/7 | INADMISSIBLE | binary mismatch |

All 22 also fail on `full_auto = 1` independently (second failing reason, not shown as a separate
column for space — `all_failing_reasons` in `baselines/fixtures.json` carries both per fixture).
Every one also passed the tool's internal self-consistency checks (`magic` match, `file_bytes ==
header_bytes + iq_bytes`), so the audit is not merely "couldn't be read" — these are genuine,
fully-parseable captures, just not producible by the current binary in the current mandatory mode.

All 22 are exactly `316268528` bytes — matches the recorder's own bound arithmetic (§3) for a
4-antenna, 273 PRB/122.88 Msps geometry, cross-checked below.

### Recorder bound check (brief deliverable 2 requirement)

`REPLAY_FRAMES(16) * samples_per_frame * nb_antennas_rx(4) * sizeof(c16_t=4B) <= 512 MiB`
(`nr_passive_replay_capture.c:19-23,107-108`). Every read header reports `iq_bytes=314572800`,
`job_bytes=384`, `fp_bytes=1400752`, giving `header_bytes=1695728` and total file size
`316268528 = header_bytes + iq_bytes`. Back-solving `iq_bytes / 16 / 4 / 4 = 1,228,800
samples_per_frame`, which is exactly `122.88e6 * 0.01` (a 10 ms frame at 122.88 Msps) — confirms
273 PRB/122.88 Msps geometry. `314,572,800 <= 536,870,912` (512 MiB) — well within bound, ~59% of
the cap.

## 3. New bounded capture (deliverable 2)

### 3.1 REPLAY=1 opt-in added to `run_manual_x410.sh` (untracked, another session's file — NOT committed)

```diff
--- run_manual_x410.sh (before)
+++ run_manual_x410.sh (after)
@@ -77,12 +77,15 @@
 trap 'exit 130' INT
 trap 'exit 143' TERM
 cd "$BUILD"
+replay_env=()
+[[ ${REPLAY:-} == 1 ]] && replay_env=(ISAC_PASSIVE_REPLAY_CAPTURE="$OUT/replay.bin" ISAC_PASSIVE_REPLAY_FAILURES=1)
 env -i PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin \
   ISAC_RX_MRC_MODE=2 ISAC_UL_RX_BRANCH=-1 \
   ISAC_DMRS_FO_APPLY=0 ISAC_SFO_CORRECT=0 ISAC_RX_BRANCH_FO=0 ISAC_RX_GAIN_TRIM=0,0,0,0 \
   ISAC_DISC_NO_RESYNC=0 ISAC_RF_STALL_MAX_REINIT=0 ISAC_CFO_TRACK_HZ=1 ISAC_CFO_TRACK_PERIOD=20 \
   ISAC_PDCCH_TIMING=1 ISAC_PUSCH_TIMING=1 ISAC_PUSCH_DIAG=1 \
   ISAC_UL_TA_SWEEP=0:0:0 ISAC_SENSE_COMB=0 ISAC_TSYNC_RESET=0 \
+  "${replay_env[@]}" \
   LD_LIBRARY_PATH="$BUILD:/usr/local/lib" \
   taskset -c 0-7 "$BUILD/nr-uesoftmodem" \
   --usrp-args type=x4xx,addr=192.168.20.2,mgmt_addr=128.178.122.174 \
```

Verified byte-for-byte equivalence when `REPLAY` is unset by rendering the resulting `env -i ...`
argument list (via a throwaway copy substituting a plain `env | sort` for the real
`nr-uesoftmodem` invocation, same variable list, `BUILD`/`OUT` set to dummy paths):

```
$ diff <(bash test_replay_env.sh) <(REPLAY=1 bash test_replay_env.sh)
4a5,6
> ISAC_PASSIVE_REPLAY_CAPTURE=/tmp/fakeout/replay.bin
> ISAC_PASSIVE_REPLAY_FAILURES=1
```
Exactly the two intended variables, nothing else differs — REPLAY unset reproduces the pre-edit
environment identically.

### 3.2 Pre-launch checks

```
$ pgrep -a -x nr-uesoftmodem; pgrep -a -x nr-softmodem   # both empty (exit 1)
$ timeout 10s uhd_find_devices --args type=x4xx,addr=192.168.20.2,mgmt_addr=128.178.122.174
...
    claimed: False
    fpga: UC_200
    serial: 327C1F2
$ find openair1 openair2 executables radio -path '*/tests/*' -prune -o \
    \( -name '*.c' -o -name '*.h' -o -name '*.cpp' -o -name '*.cc' \) \
    -newer cmake_targets/ran_build/build/nr-uesoftmodem -print -quit | grep -q . \
    && echo BLOCK || echo "NOT BLOCKED: binary is current"
NOT BLOCKED: binary is current
$ sha256sum cmake_targets/ran_build/build/nr-uesoftmodem cmake_targets/ran_build/build/liboai_usrpdevif.so
b064c9957bc1b68d717543eb2bac28c4b2616115bc675f5fed38685f008742fe  nr-uesoftmodem
ad0a71c56dbc509149337460af7d0e97c64d4f390f85bec48b8882aa64f64583  liboai_usrpdevif.so
```
Both match P01's manifest exactly. `adaptive_manual_dlul.conf` confirmed
`pdcch_blind_monitor_full_auto = 0` (line 11), `autoconf = 0` (line 9), `autodiscover = 0` (line 10).

Port 8083 (the script's `MONITOR_PORT` default) was already held by another session's leftover
dashboard (`root 3108932 .../adaptive-rx-UL-DL/tests/passive_rx/monitor/monitor.py ... --port
8083`, connected to a different tree's report stream — not a radio process, not touched). Used
`MONITOR_PORT=8090`/`8091` instead (a free port, exactly the documented `MANUAL_DL_UL.md` escape
hatch: "Set MONITOR_PORT when another dashboard uses 8083"). The shared `flock`
(`/tmp/adaptive-rx-UL-DL.radio.lock`) was confirmed free before each attempt; no other session's
radio process or lock was touched.

### 3.3 Attempt 1 — VOID

```
$ sudo -n env REPLAY=1 DURATION=180 MONITOR_PORT=8090 bash tests/passive_rx/run_manual_x410.sh
OUTPUT=/home/sens/NICOLA/captures/sensing_manual_fixed.XmtvnI
MONITOR=http://localhost:8090/
verdict=VOID_RF_OR_ASSERT cleanup=CLEAN exit=0 OUTPUT=/home/sens/NICOLA/captures/sensing_manual_fixed.XmtvnI
```
`validity.txt`/`stop_reason.txt` = `VOID_RF_OR_ASSERT`, `process_exit.txt`=0, `cleanup_status.txt`=CLEAN,
`nic_missed_before`==`nic_missed_after`=27520822 (no NIC loss), `SIB1 common facts` count = 0,
`RFSTALL` count = 2, `RXDISCONT` count = 0. `run.log` tail:
```
[HW]     SENSING: RFSTALL USRP_RX_START reason=UHD metadata error received=0 requested=61440 metadata=ERROR_CODE_OVERFLOW (Overflow)
[HW]     SENSING: RFSTALL radio read failed; refusing invalid IQ
```
Overflow at the very first `USRP_RX_START`, before any SIB1 — cold-start RF variance, not an
assertion or code defect. Kept as evidence per the memory rule (zero SIB1 = VOID, do not
over-interpret). Retried.

### 3.4 Attempt 2 — RF-valid, but replay never armed

```
$ sudo -n env REPLAY=1 DURATION=180 MONITOR_PORT=8091 bash tests/passive_rx/run_manual_x410.sh
OUTPUT=/home/sens/NICOLA/captures/sensing_manual_fixed.JPdoGb
MONITOR=http://localhost:8091/
verdict=RF_VALID_REQUIRES_DL_UL_CRC_EVIDENCE cleanup=CLEAN exit=0 OUTPUT=/home/sens/NICOLA/captures/sensing_manual_fixed.JPdoGb
```
- `validity.txt` = `RF_VALID_REQUIRES_DL_UL_CRC_EVIDENCE`, `stop_reason.txt` = `DURATION_COMPLETE`,
  `process_exit.txt` = 0, `cleanup_status.txt` = CLEAN.
- `nic_missed_before` == `nic_missed_after` = 27520822 (no NIC loss).
- `grep -c 'SIB1 common facts' run.log` = **1**.
- `RXDISCONT`/`RFSTALL`/`Assertion.*failed` counts: 0/0/0.
- `receiver.conf:11` `pdcch_blind_monitor_full_auto = 0` (confirmed manual mode).
- `checksums.txt`: `nr-uesoftmodem` sha256 = `b064c995...` (matches current build/P01 manifest),
  `liboai_usrpdevif.so` sha256 matches, `receiver.conf` sha256 =
  `a39e40f00e4ad0f456cb2b5c947f6e1e6bdbdadef069306231ef3075349ade92`.
- `sha256sum run.log` = `b9618723f3440b3832599c6b205a87d053937fdf2bac5b3c6ff6fdc48f830a6b`.
- Last `pusch_passive[` line: `try=12694 crc_ok=10562 (83.2%) seg_fail=2132 zero_tb=0 (0.0%)
  ta_refined=9178 uci[trials=21176 rescued=4939] health=83.2% unsup=1 setup_fail=0 ul_cfr[submits=0 re=0]`.
- Last `PDSCHQ` line: `queued=90774 decoded=42014 crc_ok=38583 (91.8%) dropped[full=4166
  stale=44560] max_lag_slots=180780/20`.
- `grep -aE 'REPLAY (READY|VOID)' run.log` → **NONE**. `grep -aic replay run.log` → **0** (not even
  a failure-path log line fired).
- `ls "$OUT"` → no `replay.bin` present at all (`sudo -n stat` on the expected path: "No such file
  or directory").

This run is a good, valid manual-mode OTA capture by every RF/plan criterion (SIB1 present, high
DL/UL CRC, clean exit, current binary, full_auto=0) — but it produced **zero** replay activity.

### 3.5 Root cause (why attempt 3 was not spent)

Read `nr_passive_replay_capture.c`'s `nr_passive_replay_dl()`:
```c
if (success && job->sweep_ticket.generation && (!failures || job->sweep_ticket.settled)
    && atomic_load(&ul_seen)) {
  int expected=RP_ARMED;
  atomic_compare_exchange_strong(&state,&expected,RP_REQUESTED);
}
```
(`nr_passive_replay_capture.c:206-209`) — the **only** transition out of `RP_ARMED`, and it
requires `job->sweep_ticket.generation != 0`.

`sweep_ticket` is populated **only** inside
`nr_pdcch_blind_monitor_rt.c:2100`: `if (g_pdsch_sweep_on && !is_dci10) { ... sweep_ticket =
nr_pdsch_config_sweep_select(...) ... }`. Outside that block every DL job's `sweep_ticket` is left
as declared, all-zero (`nr_pdcch_blind_monitor_rt.c:2098`:
`nr_pdsch_sweep_ticket_t sweep_ticket = {0};`).

`g_pdsch_sweep_on` is set in `pdsch_sweep_maybe_enable()`:
```c
const bool ready = cfg->dl_full_auto && (!cfg->autodiscover
    || (g_length_found && nr_pdcch_blind_monitor_autodiscover_extent_verified()));
...
g_pdsch_sweep_on = ready;
```
(`nr_pdcch_blind_monitor_rt.c:164-166,184`) — **`ready` requires `cfg->dl_full_auto` true.**

`grep -n nr_pdsch_config_sweep_select openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor_rt.c`
confirms exactly one call site (line 2108, inside the `g_pdsch_sweep_on` guard) — the only place
`sweep_ticket.generation` is ever assigned a nonzero value anywhere in the codebase.

**Conclusion, proven from source, not inferred from one run:** under the plan's mandatory manual
mode (`pdcch_blind_monitor_full_auto = 0`), `cfg->dl_full_auto` is always false, so
`g_pdsch_sweep_on` is always false, so `sweep_ticket.generation` is always 0 for every DL job, so
`nr_passive_replay_capture.c`'s recorder can **never** leave `RP_ARMED`, and `ISAC_PASSIVE_REPLAY_CAPTURE`
will **never** produce a `replay.bin`, regardless of RF quality, CRC rate, or run duration. This is
deterministic, not RF variance. A third attempt would exercise the identical code path and cannot
produce a different result; per the plan's "do not tune to get a pass" rule, it was not spent.

**Why not patched here**: this is a real design decision (what should "settled"/arm mean for a job
with no sweep in progress at all?) inside `nr_passive_replay_capture.c` /
`nr_pdcch_blind_monitor_rt.c`, both outside P02's narrow scope (a launcher env opt-in plus fixture
audit/registration/checker infrastructure). Reported as a structural blocker rather than patched.

## 4. Replay verification (deliverable 3)

**Not performed.** No admissible fixture exists (§2: all 22 pre-existing captures rejected; §3: the
new capture could not produce a `replay.bin` at all, root-caused in §3.5). There is nothing to run
`ISAC_PASSIVE_REPLAY_INPUT=... ./nr-uesoftmodem ...` against. This is recorded as **BLOCKED**, not
skipped silently.

## 5. Fixture registration (deliverable 4)

`tests/passive_rx/baselines/fixtures.json`: `status: "BLOCKED"`, `admissible_fixtures: []`, full
`blocker` object (summary/root_cause/citations — independently re-verified against the live tree,
§3.5), all 22 pre-existing candidates and both new capture attempts recorded under
`pre_existing_captures_considered` / `new_capture_attempts`.

`check_manifest.py --fixtures tests/passive_rx/baselines/fixtures.json`:
```
FIXTURES REJECTED (1):
  fixtures.json: admissible_fixtures is empty -- no reproducible baseline replay fixture is currently registered (see the file's status/blocker fields)
```
Exit 1 — correctly rejects, since G0 requires at least one reproducible case.

`check_manifest.py --selftest` (full output in the task report) adds cases (f1)/(f2)/(f3), all
`PASS`: a synthetic single-fixture registry (three tiny stand-in files, not a real capture) passes
whole; a copy with `replay_bin_sha256` altered is correctly rejected; the real shipped (empty)
`fixtures.json` is correctly rejected. Pre-existing cases (b)/(c)/(d)/(e) still `PASS` unchanged.
Case (a) ("fresh manifest passes") now **FAILs** — this is a pre-existing P01 issue, not introduced
by P02: the P01 manifest (`manifest_20260910_6fcb7a6-dirty.json`) was generated at commit
`6fcb7a6c...`, and a later commit (`e4c8cadd5f`, "P01 fix round 1") landed before this task started
— `git.commit`/`git.tracked_diff_patch_sha256` already mismatched the live tree before any P02 edit
(confirmed by running `check_manifest.py` against the manifest before touching anything, see the
task report's transcript). This task's own edits to two of the manifest's `scoped_files`
(`tests/passive_rx/run_manual_x410.sh`, `tests/passive_rx/check_manifest.py`) add two more expected
diffs on top of that pre-existing staleness. Regenerating the P01 manifest is P01's scope, not
P02's; not done here.

## 6. P01 manifest staleness (context, not a P02 defect)

```
$ python3 tests/passive_rx/check_manifest.py tests/passive_rx/baselines/manifest_20260910_6fcb7a6-dirty.json
MANIFEST MISMATCH (2):
  git.commit: manifest='6fcb7a6c319d7ae7ad4b6cb03c075de0e2f7a77a' live='e4c8cadd5f22b40605f82f6adedc838ee8e056f5'
  git.tracked_diff_patch_sha256 (recomputed live git diff --binary): manifest='e450b55e...' live='f078f2db...'
```
(run BEFORE any P02 edit). `build.nr_uesoftmodem.sha256` was NOT in the mismatch list — the binary
itself is unchanged since manifest generation, which is why §3's binary-match checks above are
still valid against this manifest's recorded hash.
