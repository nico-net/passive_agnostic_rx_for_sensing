# Passive RX live monitor

Browser dashboard for a **single** passive receiver, fed from the receiver's own DetectionReport
ZeroMQ PUB socket. Built for watching an X410 capture over SSH.

```bash
# on the capture host, after the receiver is up
./monitor.py --connect tcp://127.0.0.1:5556 --log /tmp/passive_rx/rx1.log

# from your laptop
ssh -L 8080:localhost:8080 sens6      # then open http://localhost:8080/
```

The receiver must have `[sensing] report_endpoint` set (already added to
`ue.passive.aoa1rx.100mhz.conf`). `report_path` keeps working alongside it, so offline scoring is
unaffected.

## Tabs

| Tab | Shows | Read it when |
|---|---|---|
| Link & Sync Health | CFO vs the mis-lock band, STO flywheel %, SFO correction state, CFR occupancy per source, PDSCH CRC, X410 overflows, log tail | **First.** A receiver can be alive and producing nothing real; this is where that shows. |
| DL Detections | Range–Doppler raster + CFAR markers, detection table with bearing/SNR/p(real) | Checking what the gNB-illuminated path sees. |
| UL Detections | PUSCH row counts and decode health | Checking the UE-illuminated path — see the caveat below. |
| Tracking | World map: X410 at its surveyed position, gNB, per-track bearing rays and position trails | Watching targets. |
| Config / Scene | Geometry, CPI axes, source mix, raw report | Confirming what the run actually used. |

## Two things the map depends on

**A single receiver cannot localise on range alone.** One Tx–Rx pair fixes only a bistatic
*ellipse*. The position comes from intersecting that ellipse with the AoA bearing ray
(`aoa_localize()` in `isac_aoa.cc`, the same closed form the central node uses). So:

- `aoa_enable = 1` **and** a valid `rx_array` are required, or every track is bearing-less and the
  map stays empty. A track with no bearing carries *no* `position` field at all — never `(0,0)`,
  which is the receiver itself and would plot as a real fix at the origin.
- The receiver must run with `--ue-nb-ant-rx <N>` matching the array, or AoA is silently absent.

**Range is differential (ΔR).** `aoa_localize()` expects ΔR = R_bistatic − baseline. That holds
while the direct path sits at range bin 0. If the deployment puts it elsewhere (an OTA cell does),
the fixes shift with it — reconcile the range convention before trusting a position.

## Why UL is not on the map

The uplink illuminator is a **UE**, whose position is not surveyed and changes. A PUSCH-sourced echo
therefore sits on a different bistatic ellipse from every DL source and cannot be geolocated the same
way — `nr_isac.h` says the same thing at `NR_ISAC_SRC_PUSCH_DMRS`. The UL tab reports row counts and
decode health only.

Also honest about a current limit: the engine fuses all sources into **one** grid, so a *detection*
cannot be attributed to DL or UL individually — only the per-source row counts (`src_occ`) can. The
UL tab says so on screen rather than implying otherwise.

## Checks

```bash
python3 test_monitor.py     # geometry round-trip + store behaviour, no deps beyond stdlib
```

The geometry test mirrors the C++ `aoa_localize()` closed form: place a target, compute its ΔR and
bearing, recover it. If either side drifts, this fails instead of the map quietly plotting tracks in
the wrong place.

## Known limits

- The RD raster needs `[sensing] capture = 1` for `rvm_blob` to be in the report; without it the
  raster is blank and only the detection markers/table populate.
- `--connect` is repeatable and receivers appear in the selector, but they are shown side by side,
  never fused. Cross-receiver fusion is `repos/isac`'s job and is not present on this host.
- Track bearing is the last accepted detection's, not a filtered state — position jitters at the
  bearing's own noise. Add a bearing state to the Kalman filter if that matters.
