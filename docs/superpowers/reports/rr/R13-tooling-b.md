# W4b — R13 SA-bed review fixes

[CODE-READ] Worktree `/home/nicola/NICOLA/wt/rr-w1`, branch `rr/w4-r13`, reviewed baseline `ba80f72207`. Host of this work: DGX aarch64; target bed: sens6 x86_64. `PROJECT_MEMORY.md` and frozen sens6 paths are unchanged. This report supersedes the affected behavior and limits in [R13-tooling.md](R13-tooling.md).

## Findings and changes

1. **Passive rfsim dies on restart — confirmed.** [CODE-READ] `radio/rfsimulator/simulator.cpp:socketError()` previously called `exit(1)` for every client on EPOLLRDHUP/HUP/ERR. [IMPLEMENTED, NOT VALIDATED on live traffic] Reconnect is gated by client role and the existing `IS_PASSIVE_RX_MODE(get_softmodem_params())` flag, independently of `ISAC_RECONF` so both experimental controls survive. The reader removes the old socket/queued and partial packets, then re-enters `startClient()` with backoff. Nonblocking connect waits are bounded at 200 ms, retry delay is 1 s in SIGINT-aware 100 ms steps; losing the first timestamp handshake retries too. No samples are delivered while disconnected. Successful reconnection rebases to the new gNB timestamp, with a one-sample skip for an accidentally identical next timestamp, so the UE's existing RXDISCONT path sees loss. Attempts and elapsed monotonic milliseconds are logged. Dummy socket writes use MSG_NOSIGNAL only in passive mode. After reconnect, the read side supplies timestamped zero keepalives with 10 ms lead and suppresses old UE dummy writes, which still carry the previous gNB clock. Socket mutation and writes share the existing mutex. Ordinary UE socket loss still exits, and server behavior stays unchanged. The existing R8c timestamp-gap classifier decides SOFT versus HARD_REVERIFY; no epoch is injected from gNB logs or transport internals.
2. **BWP telnet requirements missing — confirmed.** [CODE-READ] `common/utils/CMakeLists.txt` defaults `ENABLE_TELNETSRV=OFF`. [IMPLEMENTED] Both CPU/GPU RUNBOOK configure commands enable it and both builds request `telnetsrv telnetsrv_ci`; `run_arm.sh` requires both `.so` files before any launch. The worker's CPU cache was also configured ON and both targets built.
3. **Recovery included operator downtime — confirmed.** [CODE-READ] The scorer already computed the first post-restart DCI as `validation_start` but subtracted `apply` for recovery. [OFFLINE VERIFIED, DGX aarch64] Both restart scenarios now subtract `validation_start`; BWP switch retains apply-based timing. JSON includes `recovery_origin` (`validation_start` or `apply`), `recovery_origin_t`, and `recovery_from_apply_s`. Missing restart truth leaves the primary time/origin timestamp null and the run INCOMPLETE. gNB DCI truth remains scorer-only. Tests cover both origins, changed request time with unchanged first DCI, missing truth, and legacy metadata.
4. **Dedicated arm differs from plan — confirmed; renamed.** [CODE-READ] `common/utils/telnetsrv/telnetsrv_ci.c:cicmds` exposes BWP switching, release, reestablishment and handover, but no arbitrary BWP-size mutation/config reload. `openair2/GNB_APP/gnb_config.c:get_bwp_config()` loads the BWP list into `nr_mac_config_t` during gNB setup; `openair2/RRC/NR/rrc_gNB.c:rrc_gNB_generate_dedicatedRRCReconfiguration()` encodes the existing reconfiguration parameters rather than reloading the file. `telnetsrv_rrc.c` exposes release operations; O1 `config`/`bwconfig` explicitly reject changes while L1 runs, and `bwconfig` changes common carrier/initial-BWP geometry, not a UE dedicated BWP. Implementing synchronized configuration mutation is not a cheap bed-only change. [IMPLEMENTED; scorer OFFLINE VERIFIED, DGX aarch64] Runner/campaign/scorer use `same_cell_restart_size_change`, requiring a DCI-size change plus in-window HARD_REVERIFY/CONTINUITY_LOSS and no HARD_RESET for enabled runs. It uses a 30 s restart recovery target and 60 s post-action epoch window. Old `dedicated_change` metadata is normalized to this name. The existing `gnb_dedicated.cfg` filename remains the size variant. This is explicitly **not** plan coverage for reattach under changed dedicated config with a continuously running gNB.
5. **Host-local traffic — credible routing defect, not measured on sens6.** [CODE-READ] Previously both the UE TUN and UDP sink/sender were in the host netns, permitting local delivery to the assigned UE IP. [IMPLEMENTED, NOT VALIDATED on live traffic] Runner creates a private UE netns and veth RF link (192.0.2.1/30 host, 192.0.2.2/30 UE), runs the UE and sink there, leaves the sender in the host, and requires host `ip route get UEIP` to use `ogstun`. Privileged creation/cleanup, subnet and firewall requirements are documented. `check_traffic.py` fails the run before `ready` unless a fresh recent ≥10 s interval reaches ≥100 C-RNTI grants/s; missing/stale/sparse metrics fail. Traffic timeout includes acquisition allowance and remains bounded; cleanup owns termination.
6. **UPF wildcard conflict — preflight/template gap confirmed.** [CODE-READ] The old preflight accepted any UDP :2152 listener and provided no UPF template; the actual sens6 service binding has not been inspected. [IMPLEMENTED, NOT VALIDATED on sens6] New `open5gs-upf.cfg` merge fragment pins GTP-U to `127.0.0.7`. Preflight requires exactly that listener, rejecting wildcard/other addresses before the gNB tries `127.0.0.100:2152`.
7. **Stable duration — corrected.** [IMPLEMENTED; scorer OFFLINE VERIFIED, DGX aarch64] Stable defaults to 900 s after `ready`; `R13_SOAK=1` selects 3600 s unless `R13_DURATION_S` overrides it. Scorer requires ≥15 minutes, retaining ≤1 false SOFT/h and zero false HARD. Thus one false SOFT in 15 minutes fails; this is not full-hour R14 evidence. Changed arms retain 180 s defaults.
8. **Cleanup and lock inheritance — confirmed.** [CODE-READ] The previous trap restored default INT/TERM behavior during cleanup and long-lived children inherited fd 9. [IMPLEMENTED] Cleanup now ignores further INT/TERM, preserves interrupt exit status, closes fd 9 before every long-lived launch (including timestamp-reader descendants), tracks root-owned UE/sink groups, and removes the private netns/veth. `env --default-signal=INT,TERM` resets Bash's ignored background SIGINT before exec, allowing Python traffic helpers to stop on SIGINT too. No SIGKILL escalation was added.

## Not-a-bug / limits on the review claims

- [CODE-READ] Missing **DEDICATED_CHANGE_SUSPECTED / SOFT** with one UE is not itself a defect: the epoch authority requires two distinct reopened converged RNTIs within 2 s. The previous report disclosed this. Renaming the arm avoids attributing its restart continuity bump to that trigger; the dedicated-only plan case remains uncovered.
- [CODE-READ] A UPF wildcard bind on sens6 is not established by this repository: the previous tooling omitted a UPF fragment. The new address check prevents the hypothesized conflict without claiming the installed service was measured.
- [CODE-READ] gNB-truth use for `validation_start` is confined to scoring and does not violate the agnostic receiver boundary. No gNB config/log values are fed to the passive receiver.

## Offline validation

[OFFLINE VERIFIED, DGX aarch64] Configure/build/test invocations (the temporary static-check script is expanded below for reproducibility) were preceded by waiting for `/home/nicola/NICOLA/wt/rr-orchestration/BUSY` to disappear and used the shared lock per command. No exclusive lock, softmodem, rfsim bed, Open5GS service, GPU build/run, or OTA measurement was executed. Configure/build logs are `/tmp/r13b-configure.log`, `/tmp/r13b-build.log`, `/tmp/r13b-build-final.log`; Python output is `/tmp/r13b-score.log`.

```bash
flock -s -w 7200 /tmp/td_measure.lock nice -n 19 cmake -S . -B cmake_targets/ran_build/build -DENABLE_TELNETSRV=ON
flock -s -w 7200 /tmp/td_measure.lock env CCACHE_DIR=/tmp/rr-w1-ccache nice -n 19 ninja -C cmake_targets/ran_build/build -j8 nr-uesoftmodem oai_usrpdevif rfsimulator params_libconfig nr-softmodem telnetsrv telnetsrv_ci tests
flock -s -w 7200 /tmp/td_measure.lock nice -n 19 python3 -m unittest discover -s tests/passive_rx/sa_bed -p test_score_r13.py -v
flock -s -w 7200 /tmp/td_measure.lock nice -n 19 bash -n tests/passive_rx/sa_bed/run_arm.sh
flock -s -w 7200 /tmp/td_measure.lock nice -n 19 python3 -m py_compile tests/passive_rx/sa_bed/{score_r13,test_score_r13,check_traffic,stamp}.py
flock -s -w 7200 /tmp/td_measure.lock nice -n 19 python3 - <<'CHECK'
from pathlib import Path
import subprocess, yaml
root = Path('tests/passive_rx/sa_bed')
for block in (root / 'RUNBOOK.md').read_text().split('```bash\n')[1:]:
    subprocess.run(['bash', '-n'], input=block.split('```')[0], text=True, check=True)
assert yaml.safe_load((root / 'open5gs-upf.cfg').read_text())['upf']['gtpu']['server'] == [{'address': '127.0.0.7'}]
CHECK
flock -s -w 7200 /tmp/td_measure.lock nice -n 19 ctest --test-dir cmake_targets/ran_build/build -j4 --output-on-failure
```

- [OFFLINE VERIFIED, DGX aarch64] Configure and all required targets passed. The telnet-enabled full target build had 49 Ninja steps; final rfsimulator rebuild passed after review changes.
- [OFFLINE VERIFIED, DGX aarch64] Initial changed scorer tests failed as expected (14 tests: 1 failure, 3 errors, old origin/schema/scenario/duration). Final **18/18** scorer/smoke tests passed.
- [OFFLINE VERIFIED, DGX aarch64] Runner `bash -n` passed; all RUNBOOK Bash blocks parse; four Python files compile; UPF YAML parses and its address assertion passes (`/tmp/r13b-static-checks.py`, a temporary syntax/parse check, no service startup).
- [OFFLINE VERIFIED, DGX aarch64] Full CTest ran once: **148/157 passed**, 9 failed, **1600.87 s**, exit 8. `nr_td_sim_test` passed. Failures are exactly the documented ARM/intermittent set (`test_nr_pusch_ra0_qam64`, `test_nr_pusch_ra0_qam256`, `dft_test`, `test_nr_modulation`) and sandbox socket set (`time_management_tests`, `test_gtp`, `test_vrtsim`, `test_vrtsim_cirdb`, `nr_cuup_functional_test`); no new failures. Log: `/tmp/r13b-ctest.log`.
- [OFFLINE VERIFIED, DGX aarch64] `git diff --check`, the exact sens6 frozen gate from the brief, and `git diff --quiet -- PROJECT_MEMORY.md` passed; the frozen gate is repeated immediately before commit.

## DEFERRED MEASUREMENTS

[PLANNED, sens6 x86_64] Build/provision using [RUNBOOK.md](../../../../tests/passive_rx/sa_bed/RUNBOOK.md) §2–§3. There is no rfsimulator test target in `radio/rfsimulator/CMakeLists.txt`; §8 documents the permitted manual reconnect check instead. With `R` and `BUILD` set to the sens6 checkout/build, exact first checks are:

```bash
mkdir -p /tmp/r13b-bwp && cd /tmp/r13b-bwp
R13_DURATION_S=180 bash "$R/tests/passive_rx/sa_bed/run_arm.sh" bwp_switch sa on
python3 "$R/tests/passive_rx/sa_bed/score_r13.py" .
mkdir -p /tmp/r13b-same && cd /tmp/r13b-same
R13_DURATION_S=180 bash "$R/tests/passive_rx/sa_bed/run_arm.sh" same_cell_restart_size_change sa on
rg 'reconnect attempt=|reconnected after|RXDISCONT|CONFIG_EPOCH' rx/rx.log
python3 "$R/tests/passive_rx/sa_bed/score_r13.py" .
mkdir -p /tmp/r13b-pci && cd /tmp/r13b-pci
R13_DURATION_S=180 bash "$R/tests/passive_rx/sa_bed/run_arm.sh" cell_restart sa on
rg 'reconnect attempt=|reconnected after|RXDISCONT|CONFIG_EPOCH' rx/rx.log
python3 "$R/tests/passive_rx/sa_bed/score_r13.py" .
```

[PLANNED] Check same passive PID across disconnect, paced attempts, RXDISCONT then CONTINUITY_LOSS, fresh convergence, PCI-changing HARD_RESET, SIGINT during downtime, repeat reconnect, normal active-UE exit on socket loss, route/TUN traffic, smoke threshold, and lock release after interrupted cleanup. Repeat all SIB1/flag variants in fresh directories and the five-repetition campaign in RUNBOOK §5. Supply TD truth audit only after reviewing actual ground truth. GPU sens6 runtime and x86 builds remain deferred.

[PLANNED, DGX aarch64] Standard 4-RX regression, **orchestrator only**, in an idle slot; use fresh OUT directories:

```bash
cd /home/nicola/NICOLA/wt/rr-w1
for flag in 0 1; do
  flock -x -w 7200 /tmp/td_measure.lock env -u ISAC_RX_BRANCH_FO ISAC_RECONF=$flag BUILD=/home/nicola/NICOLA/wt/rr-w1/cmake_targets/ran_build/build CELL='-C 3319680000 -r 106 --ssb 516' RXEXTRA='--ue-nb-ant-rx 4' GNBARGS='-m 9 -n 0 -M 106 -l 1' GATE_SECS=420 GATE_MODE=postconv GATE_NCTX_MIN=2 GATE_REOPENS_MAX=0 GATE_POSTCONV_CRC_MIN=99.8 GATE_POSTCONV_MIN_DEC=5000 GATE_TTC_MAX_TDA0=39 GATE_TTC_MAX_TDA2=77 GATE_CRC_FLOOR=93.4 GATE_DROP_MAX=2.5 OUT=/tmp/rr-r13b-reconf-$flag bash tests/passive_rx/dgx/rfsim_regress.sh 1
done
```

## Open issues

[IMPLEMENTED, NOT VALIDATED on live traffic] Reconnect transport/flow-control, namespace setup, telnet trigger, repeated restarts and signal cleanup need the deferred manual checks; passing builds and scorer fixtures do not establish receiver recovery. A reconnect coinciding with acquisition (rather than tracked reception) may not emit a tracking RXDISCONT; the runner deliberately waits for baseline lock/convergence before applying a scenario. The real dedicated-only plan case and two-UE trigger coverage remain open. [KNOWN ISSUE, inherited] sens6 GPU runtime, rank >1 CB0 admissibility, ~3 CB0 items/grant, and scan-queue drops remain unverified/unresolved; BC9c/BC10/BC11/BC12b remain deferred. No acceleration defaults or receiver inference were changed here.
