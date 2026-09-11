# Manual X410 receiver with the sensing pipeline

Build the target repository, not the acquisition experiment:

```sh
cmake --build cmake_targets/ran_build/build --target nr-uesoftmodem oai_usrpdevif -j4
sudo -n env DURATION=120 bash tests/passive_rx/run_manual_x410.sh
```

The launcher refuses an occupied radio or dashboard port. It never changes the NIC,
resets the X410, starts a transmitter, or reads gNB configuration. The existing
`run_adaptive_receive_test.sh` and detector source are unchanged.

This is explicitly a deployment-specific manual profile: carrier, bandwidth,
numerology, SSB, CORESET and UL layout are supplied. CFO/drift CLI seeds are the
existing manual values, not newly measured acquisition results. Automatic carrier
acquisition and layout sweeps are off. DL enables DCI 1_0 and an explicitly
reconciled 47-bit DCI 1_1 layout: no BWP-indicator bit and one TDA-index bit.
The two TDA entries (S=1, L=13/7), additional DMRS position 2, and MCS table 1
come from the prior passive CRC-backed capture `auto_ul_evidence_retry.G8BYYc`,
configuration `e65ce1703ffffe66`, not gNB configuration. They are a manual
waveform interpretation, not a claim that every dedicated RRC field is known.
Uplink retains the existing manual 0_1 layout. CSI-RS is not invented without its
resource configuration. The detector stays enabled; this test does not validate
array calibration, target geometry, or range/Doppler scoring.

Outputs include copied configuration, binary checksums, NIC counter values,
receiver logs, raw sensing reports, stop reason and a separate cleanup status.
The watchdog compares values read from sysfs, not the apparent sysfs file size.
RF faults, incomplete captures and forced shutdowns must not be used to claim CRC
performance. `RF_VALID_REQUIRES_DL_UL_CRC_EVIDENCE` is not a decoding pass verdict.
Dashboard percentages use completed transport blocks; queue drops stay separate.

The dashboard remains available after the bounded capture and marks old data stale.
For Windows through the existing VPN:

```powershell
ssh -N -L 8083:127.0.0.1:8083 sens@128.178.122.140
```

Open `http://localhost:8083/`. Set `MONITOR_PORT` when another dashboard uses 8083.
