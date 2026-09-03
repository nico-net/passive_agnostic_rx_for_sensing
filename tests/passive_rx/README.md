# Passive receiver alongside a real connected UE

Three processes, one cell, one downlink:

| process | role |
|---|---|
| `nr-softmodem` | SA gNB, rfsimulator **server**, NGAP-attached to a local open5gs core |
| `nr-uesoftmodem` | **ACTIVE UE** — normal attach (RA → RRC → NAS registration → PDU session), carries IP traffic |
| `nr-uesoftmodem --passive-rx` | **PASSIVE UE** — never transmits, never attaches, senses the active UE's CSI-RS |

Other docs in this directory:

| doc | covers |
|---|---|
| `README.100mhz_tuning.md` | 273 PRB / 100 MHz bring-up: `--ssb`, `-C`, the confs, and the traps already paid for |
| `README.upa.md` | the 2x2 UPA array geometry |
| `README_OTA.md` | over-the-air operation against the real cell |
| `ota/README.md` | the OTA sub-harness (`_run_catchrate.sh`, `_run_repeatability.sh`) |
| `captures/` | the capture harness (`run_arm.sh` and the A/B runners), symlinked from `/home/sens/NICOLA/captures` |

Run it:

```bash
cd /home/sens/NICOLA/openairinterface5g
./tests/passive_rx/run_passive_rx.sh [duration_s] [out_dir]     # default 90 s, /tmp/passive_rx
```

## Why this works at all

Two independent facts make it possible, both source-verified rather than assumed:

1. **The rfsimulator server broadcasts the downlink to every client.**
   `radio/rfsimulator/simulator.cpp`'s `rfsimulator_write_internal()` loops over all
   `t->buf[i].conn_sock` and `fullwrite()`s the *same* sample block to each. A second UE connecting
   to the same server therefore receives bit-for-bit the downlink the first UE receives — including
   PDCCH, PDSCH and CSI-RS addressed to the first UE. There is no per-UE filtering anywhere in the
   simulated air interface, exactly as on a real one.

2. **`--passive-rx` parks MAC in `UE_RECEIVING_SIB`.** Every uplink trigger in `nr_ue_ul_scheduler`
   is gated on `state >= UE_PERFORMING_RA`, so nothing is ever transmitted. See
   `TOTAL_PASSIVE_UE_HANDOVER.md` Phase 1.

**The CSI-RS the passive receiver senses exists *because* the active UE is connected.** The gNB
schedules NZP-CSI-RS per connected UE (`gNB_scheduler_primitives.c`'s `nr_csirs_scheduling` walks
`UE_info` and each UE's `csi_MeasConfig`). With no UE attached there is no CSI-RS on the air at all.
So this is a genuine demonstration of a receiver exploiting *another* UE's downlink reference
signal with no RRC context of its own — the intent of `TOTAL_PASSIVE_UE_HANDOVER.md` Phase 2.

## Prerequisites

- **open5gs running locally**, AMF NGAP on `127.0.0.1:38412`. The script checks and exits if absent.
- The subscriber in `ue.active.conf` provisioned in the open5gs mongodb. Check with:
  ```bash
  mongosh --quiet --eval 'JSON.stringify(db.getSiblingDB("open5gs").subscribers.findOne({},{imsi:1,security:1,_id:0}))'
  ```
  The shipped config uses IMSI `001060123456743` (PLMN 001/06, matching `/etc/open5gs/amf.yaml`).
- **Passwordless sudo** — the *active* UE needs `CAP_NET_ADMIN` for its `oaitun_ue1` interface. The
  passive UE does not and runs unprivileged.

## Why NOT `tests/sensing_sim`

That harness is `--do-ra`-based, and `--do-ra` is mutually exclusive with `--passive-rx` (which is a
modifier of SA and needs the SA cell-search / MIB / SIB1 path). A `--do-ra` gNB is not a usable
illuminator for a passive receiver. Hence a separate directory rather than another scene there.

## Results (2026-07-27, 90 s runs)

**Reliable, every run:**

| | |
|---|---|
| active UE | attaches, IP from the core, 433 pings 0 % loss, ~420-500 UL HARQ rounds seen by the gNB |
| passive UE | syncs, decodes SIB1, **0** PRACH / RAPROC / Msg3 / PUCCH / PUSCH log lines |
| passive UE capture | `occ[csi=32 dmrs=0 data=0]` in **every** CPI — all 32 slow-time rows filled from `csirs_monitor` |
| reported axes | CPI 2.56 s, `range_res` 7.86 m, `vel_max` ±0.564 m/s — all consistent with a 160-slot CSI-RS period |

`dmrs=0 data=0` is the expected and correct result, not a failure: in `UE_RECEIVING_SIB` the MAC
configures no PDCCH/PDSCH, so `csirs_monitor` is the only tap that can fire.

**Not reliable — read before trusting the sensing output:** detection of the *synthetic* target in
`[sensing_channel]` is intermittent. Across four 90 s runs it appeared in the correct range bin
(11–12, i.e. 86–94 m against a 88–95 m ground truth, so within one bin when present) in
**3/10, 0/8, 3/10 and 0/8 CPIs**. Range accuracy is good when it detects; detection *rate* is not.
Treat this harness as validated for **capture**, not yet for **detection performance**.

### Known artifacts

- **A dense detection cluster at 760–825 m dominates almost every CPI** (17–21 dB, usually the
  strongest detection). It sits just below the comb-12 range de-aliasing clip,
  `nof_range/comb = 1272/12 = 106 bins = 833 m` (CLAUDE.md's per-row de-aliasing note).

  **Root cause, established 2026-07-28 (was "unresolved" before):**
  - It is **not** an interpolation artifact — identical with `interpolate=0` (raw comb-12, no
    frequency gap-fill) and `interpolate=1`.
  - It is **not** from the synthetic `[sensing_channel]` target/LOS model — identical with
    `sensing_channel.enable=0` (pure passthrough of the real air interface, nothing injected).
  - It **is** real content in the comb-12-reconstructed CFR: a wide low-lying lobe that rises as
    range approaches the comb-12 aliasing boundary (833 m) from below. Confirmed by widening the
    per-row de-alias taper (`[sensing] dealias_taper_bins`, new knob, default 4) to 24 then 60 bins:
    the artifact's *fraction* of detections did not shrink (stayed 45–49%); its *peak bin* simply
    moved inward with each widening. A post-hoc range-domain taper can only delete whatever falls
    inside its window — it cannot suppress a lobe that extends further back than the window reaches,
    so widening it just exposes the next-highest point of the same lobe. **Widening this taper is
    not the fix here** (the knob is real infrastructure for cases where the artifact genuinely IS a
    narrow edge leak — see its comment in `defs_nr_UE_ISAC.h` — this scene's artifact isn't that).
  - `clutter_removal="eca+"` (source-level clutter/direct-path cancellation, not post-hoc masking)
    **does** suppress it: 45–55% → ~17% of detections at default settings. But its default
    `eca_doppler_max_mps = 0.5` directly covers this scene's ~0.171 m/s target velocity, so ECA's
    projection removes the target along with the artifact (0/16 CPIs with a target-band detection).
    Narrowing `eca_doppler_max_mps` to 0.1 (below the target's velocity, on the theory that a
    tighter clutter band would spare it) made **both** metrics worse — ECA's actual delay/Doppler
    coupling isn't the simple separable model that predicts, so this needs reading
    `openair1/PHY/NR_UE_ISAC/eca_clutter.cc` before another parameter guess, not more tuning by trial.
  - `far_harmonic_reject` and `conj_image_reject` were also tried; neither touches it (see
    `ue.passive.conf`'s DSP-options comment block for the full list of six things tried and why).

  **Tried and RULED OUT (2026-07-28): more gNB DL antenna ports.** The theory was that more ports
  would move CSI-RS off row-2/comb-12 onto a denser row, pushing the comb-12 aliasing boundary
  (833 m) further out. Tested empirically: `gnb.sa.rfsim.conf` set to `pdsch_AntennaPorts_XP = 2`
  (+ `nb_tx`/`nb_rx = 2`, matching the in-repo `gnb.sa.band78.273prb.rfsim.2x2.conf` precedent),
  `csirs_monitor` updated to `row=3` (2-port, fd-CDM2) — active UE still attached fine (single
  antenna against a 2-port gNB is normal 3GPP behaviour, not a capability mismatch). Result: the
  live-measured `comb=` in the SENSING log was **still 12**, unchanged, and the artifact fraction
  was unchanged too (40% vs the 43–55% baseline range). Root cause, found by reading
  `nr_csi_rs_channel_estimation()` in `csi_rx.c`: CDM2-in-frequency accumulates BOTH paired REs into
  the SAME single per-RB storage slot (`csi_rs_ls_estimated_channel[ant][port][kinit_tx]`, indexed
  by RB not by subcarrier) — going to more ports via CDM adds more *ports*, not more range-resolving
  *frequency samples per RB*. It stays at exactly 1 estimate/RB regardless of port count for every
  case `get_nzp_csi_rs_resource()` implements (1/2/4/8/12 ports all use `density.present = one`).
  **The only thing that actually increases per-RB density is the `density` field itself (row-1 at
  1 port uses `density=three`, i.e. 3 REs/RB, comb-4) — and `get_nzp_csi_rs_resource()`'s switch has
  no code path that reaches row-1; `case 1` (1 port) is hardcoded to row-2/`density=one`/comb-12
  unconditionally.** Getting a genuinely denser CSI-RS resource in this codebase would need a code
  change to `get_nzp_csi_rs_resource()` (add a row-1/density-3 path), not a config change — a
  materially bigger, riskier change than this test scope justified. Not attempted.
- Six DSP option combinations were tried and none give a net win — see the comment block in
  `ue.passive.conf` for the outcome of each. Do not re-enable them expecting a fix without new
  evidence.
- **Velocity ambiguity is the binding constraint on this configuration**, and it is physics, not a
  bug. CSI-RS fires every 160 slots = 80 ms → slow-time PRF 12.5 Hz → unambiguous bistatic velocity
  only ~±0.28 m/s. The first version of this scene used a 5 m/s target, which folded ~12× and was
  undetectable; the shipped trajectory is deliberately ~0.17 m/s. Anything faster needs a denser
  reference, which is the argument for Phase 3 (blind PDCCH → other UEs' PDSCH DM-RS, per scheduled
  slot rather than per 80 ms).

## Re-deriving `csirs_monitor` if you change the gNB config

`ue.passive.conf`'s `csirs_monitor` string must match what the gNB actually transmits **exactly** —
a wrong `scramb_id` or `freq_domain` means `Ĥ = Y/X` uses the wrong `X`, giving garbage rather than
a weak signal. The shipped value `2:0:106:1:13:0:0:2:0:160:0` is derived in a comment in that file
from `nr_radio_config.c`'s `get_nzp_csi_rs_resource()`, and the period/offset were *measured*:

```bash
# start the gNB with NR_MAC debug (note: NR_MAC, not MAC) and read the schedule directly
nr-softmodem -O tests/passive_rx/gnb.sa.rfsim.conf --rfsim --log_config.nr_mac_log_level debug
grep "Scheduling CSI-RS in frame" gnb.log     # -> every 8 frames at slot 0 = 160 slots, offset 0
```

Re-derive after any change to `physCellId`, `dl_carrierBandwidth`, the TDD pattern, the number of
gNB DL antenna ports, or `do_CSIRS`.

## Files

- `gnb.sa.rfsim.conf` — SA gNB, band 78, 106 PRB, PCI 0, `do_CSIRS = 1`, PLMN 001/06, AMF
  `127.0.0.1`. Derived from `ci-scripts/conf_files/gnb.sa.band78.106prb.rfsim.conf`.
- `ue.active.conf` — credentials matching the open5gs subscriber.
- `ue.passive.conf` — `[sensing]` + `csirs_monitor` + a synthetic `[sensing_channel]` target.
- `run_passive_rx.sh` — orchestration, ordering, traffic, teardown, summary.

The teardown matches processes on their **config paths**, so a concurrent `tests/sensing_sim` run on
the same machine is never touched.
