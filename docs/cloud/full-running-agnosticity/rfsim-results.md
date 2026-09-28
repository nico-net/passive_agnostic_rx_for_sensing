# RFsim validation campaign — results

Status: **BLOCKED before Arm 0**. No arm was run.

## Environment check performed (sens6, 2026-09-26)

Per the task's core-startup step: "Start it the way the harness README/scripts document... If you
cannot start it without guessing, stop and report BLOCKED with what you found."

- `tests/passive_rx/run_passive_rx.sh` and `tests/passive_rx/README.md` both state the prerequisite
  as "open5gs running locally, AMF NGAP on 127.0.0.1:38412" and "the subscriber in `ue.active.conf`
  provisioned in the open5gs mongodb" — **neither document contains an install or start command**,
  they only assert it as a precondition the script checks (`run_passive_rx.sh:229`:
  `ss -lS | grep 127.0.0.1:38412` → exits with an error if absent).
- `ss -lnp | grep 38412` → no listener.
- No open5gs binaries anywhere on the filesystem: `find / -iname 'open5gs*'` and explicit checks for
  `amfd`/`smfd`/`upfd`/`nrfd`/`ausfd`/`udmd`/`pcfd`/`nssfd`/`scpd`/`bsfd` all came up empty except one
  unrelated kernel-tracing source-header hit (`linux-headers-*/kernel/trace/rv/monitors/scpd`, not
  open5gs). No `dpkg`/`apt` package, no snap.
- No `/etc/open5gs/` directory (README references `/etc/open5gs/amf.yaml` for the PLMN — doesn't
  exist).
- No mongodb/mongosh installed, no `/var/lib/mongodb`, no `mongod` process or unit — the
  subscriber-provisioning step the README describes has nothing to provision into.
- No `docker`/`podman`/`docker-compose` installed at all (`docker: command not found`) — so the one
  in-tree containerized open5gs (`/home/sens/NICOLA/ocudu/docker/open5gs/`, a Dockerfile + entrypoint
  script + `add_users.py`) cannot be started either without first installing a container runtime,
  which is itself outside "start it the way the harness documents."
- `/home/sens/qcore` is a *different*, unrelated Rust-based 5GC project on this host ("QCore",
  explicitly marked "on pause" in its own README) — not open5gs, not configured with this harness's
  PLMN (001/06) or subscriber IMSI (001060123456743), and nothing ties it to `tests/passive_rx/`.
  Standing it up in open5gs's place would be a guess, not a documented start path.
- Checked `root`'s and `sens`'s shell history for any prior open5gs/AMF/mongo start commands on this
  host — none found. No tmux/screen sessions holding a previously-started core. No systemd unit
  (user or system) named anything 5GC-related.
- gNB/UE binaries and `librfsimulator.so` themselves ARE present and correct per the task's pointers
  (`adaptive-rx-UL-DL/cmake_targets/ran_build/build/nr-uesoftmodem`,
  `agn-wt/gnbtest/cmake_targets/ran_build/build/nr-softmodem`) — not re-verified against the newest
  commit string since the campaign never reached a point where that mattered.

**Conclusion**: standing up a working open5gs (or equivalent) core — installing the packages,
writing `/etc/open5gs/amf.yaml` with the right PLMN, starting mongod, provisioning the
`001060123456743` subscriber — is not documented anywhere the task pointed at (context.md,
rfsim-validation-plan.md, gnbtest-report.md, the `tests/passive_rx/README*.md` set, or memory) and
is a nontrivial, error-prone guess (PLMN/APN/subscriber-key values all matter for the UE to attach).
Per the task's explicit instruction, stopping here rather than guessing.

## Arms

| Arm | Knob | Validity | Result |
|---|---|---|---|
| 0. Baseline | — | — | **NOT RUN** — blocked on core |
| 1. Rank 4 | — | — | NOT RUN |
| 2. Non-zero BWP start | — | — | NOT RUN |
| 3. AL1-only dedicated SS | — | — | NOT RUN |
| 4. AL16 | — | — | NOT RUN |
| 5. PRG=4 negative control | — | — | NOT RUN |
| 6. (interleaved VRB — plan §2 numbers this 5, not in the task's arm list) | — | — | SKIPPED (not in task's arm list) |
| 7. DL scrambling IDs ≠ PCI | — | — | NOT RUN |
| 8. UL scrambling IDs ≠ PCI | — | — | NOT RUN |
| 9. PDSCH type B, k0=2 | — | — | NOT RUN |
| 10/11. CSI-RS 8-port / 12-port | — | — | NOT RUN |
| 12. qam256 | — | — | NOT RUN |
| 13. Multi-UE (3 active UEs) | — | — | NOT RUN |
| VRB interleaving | n/a | n/a | SKIPPED per task instruction — gNB never interleaves on TX (confirmed again by `gnbtest-report.md` knob 2: RRC IE + DCI bit are real, but `gNB_scheduler_dlsch.c:962` hardcodes `VRBtoPRBMapping = 0` and `nr_dlsch.c:689` has no interleaved RE-mapping code path at all — signalling-only, no physical effect to test). |

No arm reached the validity-verdict stage (no sync attempt possible without an active UE, and no
active UE can attach without a core), so nothing here is scored as a feature PASS/FAIL — every row
is a hard prerequisite BLOCKER, not a feature result.

## What would unblock this

Someone with authority to install packages/provision the core needs to either:
1. `apt install` (or build from source) open5gs, write `/etc/open5gs/amf.yaml` for PLMN 001/06,
   start mongod + the open5gs daemons, and provision IMSI `001060123456743` per
   `tests/passive_rx/README.md`'s documented `mongosh` check; or
2. Install docker and bring up `ocudu/docker/open5gs/` (has its own entrypoint + `add_users.py`
   subscriber provisioning), pointing the gNB's NGAP config at wherever that container's AMF binds
   (need to confirm it can reach `127.0.0.1:38412`, since the container's default networking may not
   land on localhost as the harness expects).

Once `ss -lnp | grep 38412` shows a listener and the subscriber is confirmed provisioned, this
campaign can proceed exactly as specified (baseline arm first, sequential arms, `Monitor`-driven
convergence waits, gNB-log ground truth, VOID/retry on invalid runs).
