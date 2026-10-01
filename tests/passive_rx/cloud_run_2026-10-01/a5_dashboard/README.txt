A5 dashboard: Receiver health tab, headless Chromium screenshot (receiver_health.png, 1280x1500).
Evidence label: [SIM VERIFIED, cloud x86 Xeon-2.8GHz-4c, 2026-10-01, 0d04761969 (+fix cf8e2e79d7)] against the finished run
/tmp/claude-0/campaigns/20261001_rfsim_smoke/runs/002_base (metrics.jsonl + obs.jsonl, run already finished,
so metrics_age_s is large and the tile correctly shows "receiver silent"; grants_per_s 161 is this host's rfsim rate, not the DGX 380).
Command: monitor.py --metrics <run>/metrics.jsonl --obs <run>/obs.jsonl --log <run>/rx.log --port 18080
Tests: test_health.py 10/10 OK, test_monitor.py all checks passed.

curl /health (rates and obs part; metrics object abbreviated, acq_state DL_CONVERGED):
{
 "acq_state": "DL_CONVERGED",
 "pci": 0,
 "metrics_age_s": 203.9216649532318,
 "bad_lines": 0,
 "rates": {
  "crc_pct_window": 100.0,
  "grants_per_s": 161.24828258485425,
  "drop_full_pct": 0.7504428078679967
 },
 "obs": {
  "dl_per_s": 154.0641570459961,
  "ul_per_s": 0.0,
  "top_rnti": [
   [
    4660,
    2000
   ]
  ],
  "prb_hist": [
   0,
   0,
   0,
   2000,
   0,
   0,
   0,
   0,
   0,
   0
  ]
 }
}
