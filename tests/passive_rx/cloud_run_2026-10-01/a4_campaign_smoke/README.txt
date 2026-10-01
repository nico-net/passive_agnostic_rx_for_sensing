Task A4 Step 6 - real rfsim campaign smoke via campaign.py (cloud host).
[SIM VERIFIED, cloud x86 Xeon-2.8GHz-4c, 2026-10-01, 2547ff90aa]
Command: campaign.py new --name rfsim_smoke ... ; campaign.py run $C --arm base --secs 150 -- tests/passive_rx/dgx/rfsim_arm.sh ./arm 150 ; campaign.py summarize $C
Run 002_base: verdict VALID, crc 95.45 %, drop_full 1.39 %, ttc 4.22 s, CONVERGED 2, bank len 46, CPU 202 %, RSS 0.90 GB;
  metrics.jsonl 7 lines (last acq_state DL_CONVERGED, pdschq_crc_ok 16351/17130, obs_pushed=obs_written=17041, obs_dropped 0);
  obs.jsonl 19119 lines (7.6 MB, not committed; first 50 lines in 002_base.obs_head50.jsonl).
Run 001_base: first attempt whose runner process was killed externally before the child produced output (the agent's
  tool call ended); run.json stayed "running", rx.log empty. summarize/verdict classified it INTERRUPTED
  ("stale running (runner died)") - an unplanned real-world check of the power-loss path of Review Focus 3.
Raw campaign dir: /tmp/claude-0/campaigns/20261001_rfsim_smoke (not in git).
