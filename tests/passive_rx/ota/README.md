# OTA configs for the DCI format 1_0 work (2026-08-21)

Live-cell configs used to validate DCI format 1_0 on the X410 passive receiver. Every geometry value
here was read off the gNB or the MIB, never guessed — see `docs/PASSIVE_RX_ONLY_HANDOVER.md` §§10.0–10.4
for the measurements, and for four claims that were retracted along the way.

| conf | scans | purpose |
|---|---|---|
| `nrue.passive_rx.conf` | dedicated CORESET, 1_1 + 1_0 | baseline; `dci10 = "1"` |
| `nrue.passive_rx.coreset0.conf` | CORESET#0, 1_0 only, `sib1=1` | SIB1 / SI-RNTI — 100 % PDSCH CRC |
| `nrue.passive_rx.coreset0.sensing.conf` | as above + sensing on | exercises the data-aided `mod_idx` invariant |
| `nrue.passive_rx.ratc.conf` | CORESET#0, gates off, persistence off | Msg2/Msg4 — needs airplane-mode toggles |
| `nrue.passive_rx.uss_nogate.conf` | dedicated CORESET, gates off, AL2+AL4 | row-rate characterisation |

## Traps these configs encode

- **`rnti_persist_k` must be 1 to catch Msg2.** An RA-RNTI is sent once per attach, so K=2 drops it
  by construction.
- **`energy_adapt_factor = 3.0` hides RA/TC.** They peak at 2.31x the noise floor from this receiver
  while SIB1 reaches 41-57x. Characterise with gates OFF, score with them ON — rule R7.
- **`mcs_table` must keep the DEPLOYMENT value even on CORESET#0.** Format 1_0 forces table 0 for its
  own MCS in code, but TBS_LBRM's modulation order is a CELL property, TS 38.214 5.1.3.2, and reads
  this field. Setting it to 0 gave `lbrm=950984` against the gNB's own 1277992.
- **CORESET#0 geometry is derivable, not guessable.** Run once with `ISAC_OTA_CFG=1` and it prints
  these four config lines pre-filled from the MIB.

`_run_repeatability.sh` runs 6 x 90 s back to back with the ENERGY probe alternating on/off.
**Known gap: it does NOT bracket the gNB log per run**, so rows/s cannot be normalised by offered
traffic — fix that before drawing any conclusion from its trend. See handover §10.4.
