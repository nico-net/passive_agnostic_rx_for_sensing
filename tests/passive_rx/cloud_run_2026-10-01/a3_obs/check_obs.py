#!/usr/bin/env python3
"""A3 Step 9 checks: obs JSONL vs the last ISAC_METRICS line. usage: check_obs.py obs.jsonl metrics.jsonl"""
import json, sys
obs = [json.loads(l) for l in open(sys.argv[1])]
m = [json.loads(l) for l in open(sys.argv[2])][-1]
# only records completed before the metrics snapshot (the receiver keeps decoding after it)
dl = [o for o in obs if o["dir"] == "DL" and o["t_mono_ns"] <= m["t_mono_ns"]]
n, ok = len(dl), sum(o["crc"] == 1 for o in dl)
dec, cok = m["pdschq_decoded"], m["pdschq_crc_ok"]
res = {
  "n_dl": n, "pdschq_decoded": dec, "layout_probe_share_pct": round(100.0 * (dec - n) / dec, 3),
  "n_dl_crc_ok": ok, "pdschq_crc_ok": cok,
  "obs_pushed": m["obs_pushed"], "obs_written": m["obs_written"], "obs_dropped": m["obs_dropped"],
  "n_ul": sum(o["dir"] == "UL" for o in obs), "schema_all_1": all(o["schema"] == 1 for o in obs),
}
res["PASS_n_dl_le_decoded"] = n <= dec
res["PASS_crc_ok_within_1pct"] = abs(ok - cok) <= 0.01 * cok
res["PASS_dropped_zero"] = m["obs_dropped"] == 0
print(json.dumps(res, indent=1))
sys.exit(0 if res["PASS_n_dl_le_decoded"] and res["PASS_crc_ok_within_1pct"] and res["PASS_dropped_zero"] else 1)
