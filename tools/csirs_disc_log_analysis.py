#!/usr/bin/env python3
"""Read-only CSI-RS run comparison. Missing diagnostics remain unknown, not zero.

Usage: python3 tools/csirs_disc_log_analysis.py /path/to/run.log [...]
Counters are the last complete metrics snapshot (not necessarily process exit).
DIAG samples are before selection; WINNER/AMBIGUOUS samples are selection-biased.
"""
import argparse
import collections
import json
import re
from pathlib import Path


def distribution(values):
    if not values:
        return None
    values = sorted(values)
    return dict(n=len(values), min=values[0], median=values[len(values)//2],
                p90=values[int((len(values)-1)*.9)], max=values[-1])


def analyze(path):
    counts = collections.Counter({k: 0 for k in ("confirmed", "WINNER", "AMBIGUOUS", "DIAG", "VERIFY", "EVIDENCE")})
    scores = collections.defaultdict(list)
    slots = collections.defaultdict(list)
    sweep_counts = []
    stages = collections.defaultdict(list)
    metrics = {}
    first_metrics = None
    bad_json = 0
    fep = []
    last_fep_drops = None
    last_fep_counters = {}
    with path.open(errors="replace") as source:
        for line in source:
            if "ISAC_METRICS {" in line:
                try:
                    metrics, _ = json.JSONDecoder().raw_decode(line[line.index('ISAC_METRICS {')+len('ISAC_METRICS '):])
                    if first_metrics is None:
                        first_metrics = metrics
                except ValueError:
                    bad_json += 1
            if "CSIRS_BLIND CONFIRMED" in line:
                counts["confirmed"] += 1
            match = re.search(r"CSIRS_DISC (WINNER|AMBIGUOUS|DIAG|VERIFY|EVIDENCE)\b", line)
            if match:
                kind = match[1]
                counts[kind] += 1
                fields = dict(re.findall(r"([\w/]+)=([^\s]+)", line))
                for key in ("z", "z2", "mu", "sigma"):
                    if key in fields:
                        scores[kind + '_' + key].append(float(fields[key]))
                if all(k in fields for k in ("z", "mu", "sigma")):
                    z, mu, sigma = (float(fields[k]) for k in ("z", "mu", "sigma"))
                    scores[kind + '_gate_excess'].append(z-max(2, mu+12*sigma))
                if kind == "DIAG" and "slots" in fields:
                    key = '/'.join(fields.get(k, '?') for k in ("row", "fd", "l0", "l1", "rb", "P/O"))
                    slots[key].append(int(fields["slots"]))
            if "CSIRS_DISC BATCH " in line:
                for key, value in re.findall(r"(total_us|detect_us|hyp_us|sweep_us)=(\d+)", line):
                    stages[key].append(int(value))
            fep_match = re.search(r"CSIRS_DISC FEP slots=(\d+).*feed_drops=(\d+)", line)
            if fep_match:
                fep.append(int(fep_match[1]))
                last_fep_drops = int(fep_match[2])
                last_fep_counters = {k: int(v) for k, v in re.findall(
                    r"(td_overflows|confirm_drops|confirm_wait_max_us|pending)=(\d+)", line)}
            match = re.search(r"CSIRS_DISC HYPOTHESES n=(\d+) sweep=(\d+)", line)
            if match:
                sweep_counts.append(int(match[2]))
    batches = metrics.get("csirs_disc_batches", 0)
    result = dict(run=path.parent.name, path=str(path), counts=dict(counts),
                  last_metrics={k:v for k,v in metrics.items() if k.startswith('csirs_disc_')},
                  malformed_metrics=bad_json, logged_sweeps=sum(sweep_counts),
                  last_fep_feed_drops=last_fep_drops, last_fep_counters=last_fep_counters,
                  logged_fep_slots=sum(fep), fep_slots_per_logged_drain=distribution(fep),
                  sweep_per_batch=distribution(sweep_counts),
                  sampled_batch_stages={k: distribution(v) for k, v in stages.items()},
                  metrics_precede_discovery=bool(sweep_counts and not batches),
                  batch_mean_ms=metrics.get("csirs_disc_batch_us", 0)/batches/1000 if batches else None,
                  distributions={k:distribution(v) for k,v in sorted(scores.items())},
                  hypothesis_slots={k:dict(observations=len(v), sum_reported=sum(v),
                                           distribution=distribution(v)) for k,v in sorted(slots.items())},
                  unique_slots_per_hypothesis=None,
                  note="Historical DIAG slots can overlap; sums are not independent accumulated evidence.")
    if first_metrics and metrics:
        result['metrics_span_s'] = (metrics['t_mono_ns']-first_metrics['t_mono_ns'])/1e9
    return result


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('logs', type=Path, nargs='+')
    args = parser.parse_args()
    print(json.dumps([analyze(p) for p in args.logs], indent=2))
