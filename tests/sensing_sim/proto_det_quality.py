#!/usr/bin/env python3
"""Prototype of the ADAPTIVE per-detection quality gate, validated on recorded captures.

Written before the C++ so the algorithm can be checked against ground truth without a rebuild or a
recapture. The C++ (`det_quality.{h,cc}`) must mirror this.

## Why adaptive rather than a threshold

Measurement (analyze_separability.py) showed a plain `snr_db >= 12` lifts single-receiver track
precision 47 % -> 56 % at essentially no recall cost. But an absolute dB threshold does not survive a
change of receiver gain, traffic pattern, CPI length or scene: the SNR population moves bodily. What
actually carries the information is where a detection sits in ITS OWN CPI's distribution
(`snr_rank_in_cpi` AUC 0.93, `snr_minus_cpi_median` 0.88), which is scale-free by construction.

So the null distribution is ESTIMATED ONLINE and the decision is Bayes-optimal against it, rather
than compared to a constant. The only "constant" is the 0.5 posterior decision boundary, which is not
a tuning knob -- it is the optimal rule for symmetric costs, and is exposed as a cost ratio for an
operator who wants to trade precision against recall in units they actually care about.

## The model

Per CPI, over that CPI's detections:
  1. Robust null: median and MAD of the detection SNR population, EMA-smoothed ACROSS CPIs so it
     tracks gain/traffic drift. The bulk of detections are false alarms (measured precision < 50 %),
     so the robust centre estimates the false-alarm population without assuming its parameters.
  2. z_i = (snr_i - med) / mad          -- scale-free, so z ~ N(0,1) under the null by construction.
  3. P(z | false alarm) = N(0, 1).
  4. P(z | real)        = N(mu_r, 1), with mu_r estimated ONLINE as the EMA of the mean z of the
     detections currently posteriorly classified real. Self-bootstrapping; floored so the two
     components cannot collapse onto each other.
  5. Persistence (orthogonal to SNR -- it separates within EVERY SNR quartile, AUC 0.64-0.86): the
     number of recent CPIs holding a detection in the same resolution cell. Its class-conditional
     likelihoods are also estimated online as Laplace-smoothed frequencies, so no weight is set by
     hand.
  6. Posterior by naive Bayes; admit when P(real) exceeds the decision boundary.

Everything that could have been a tuned constant is instead estimated from the running data.

Usage: proto_det_quality.py <run_dir> [run_dir2 ...]
"""
import json
import math
import sys

import numpy as np

# Resolution-scaled cell size for "the same detection reappeared" -- expressed in BINS, so it carries
# over to any bandwidth/CPI length without becoming a metre constant.
PERSIST_RANGE_BINS = 5.0
PERSIST_VEL_BINS = 4.0
PERSIST_WINDOW = 4      # how many previous CPIs are remembered
EMA = 0.15              # tracking rate for every online estimate
MIN_MU_R = 0.75         # floor on the real/false-alarm separation, so the mixture cannot collapse


class AdaptiveDetQuality:
    def __init__(self, cost_ratio=1.0):
        # cost_ratio > 1 => admitting a false alarm is costlier than losing a real detection.
        self.boundary = cost_ratio / (1.0 + cost_ratio)
        self.med = None
        self.mad = None
        self.mu_r = 1.5           # separation of the real component, in null sigmas
        # Class VARIANCES, learned online. Fixing the real class at var=1 was measured to be badly
        # wrong (its true spread is 2.5-9.5): it makes the model say "real targets look like z~2.4,
        # this one is z~0.5, so it is probably noise" and throws away weak-but-genuine targets. The
        # null variance is learned too -- MAD-standardisation only makes it 1 if the null is Gaussian,
        # and measured it is 0.8-3.7.
        self.var_r = 1.0
        self.var_0 = 1.0
        self.prior = 0.35         # P(real), tracked online
        # Laplace-smoothed class-conditional persistence counts.
        self.pc_real = np.ones(PERSIST_WINDOW + 1)
        self.pc_fa = np.ones(PERSIST_WINDOW + 1)
        self.recent = []          # rings of (range_bin, dopp_bin) from previous CPIs

    def _persistence(self, rb, db, drift_bins_per_cpi):
        """MOTION-COMPENSATED persistence.

        A static cell test silently penalises fast targets: a target moving several range bins per CPI
        never matches its own previous detection, scores persistence 0, and is treated exactly like
        noise -- which is the opposite of the truth. The detection's OWN Doppler already says how fast
        its range is changing, so where it was k CPIs ago is predictable, with no extra information and
        nothing to tune: predicted_range_bin = rb - k * (range_rate * dt / range_res).
        """
        n = 0
        for k, prev in enumerate(reversed(self.recent), start=1):
            pred_rb = rb - k * drift_bins_per_cpi
            for (pr, pd) in prev:
                if abs(pr - pred_rb) <= PERSIST_RANGE_BINS and abs(pd - db) <= PERSIST_VEL_BINS:
                    n += 1
                    break
        return min(n, PERSIST_WINDOW)

    def score(self, snrs, rbins, dbins, drift=None):
        """Posterior P(real) per detection, then update every online estimate from this CPI."""
        n = len(snrs)
        if n == 0:
            self.recent.append([])
            self.recent = self.recent[-PERSIST_WINDOW:]
            return np.zeros(0)
        snrs = np.asarray(snrs, float)

        # --- 1-2. robust null, EMA'd across CPIs so it tracks drift rather than one CPI's luck
        med_c = float(np.median(snrs))
        mad_c = float(np.median(np.abs(snrs - med_c))) * 1.4826
        mad_c = max(mad_c, 1e-3)
        self.med = med_c if self.med is None else (1 - EMA) * self.med + EMA * med_c
        self.mad = mad_c if self.mad is None else (1 - EMA) * self.mad + EMA * mad_c
        z = (snrs - self.med) / max(self.mad, 1e-3)

        # --- 5. persistence, in resolution cells
        if drift is None:
            drift = [0.0] * n
        pers = np.array([self._persistence(rbins[i], dbins[i], drift[i]) for i in range(n)], int)

        # --- 3-4 + 6. naive Bayes posterior
        mu = max(self.mu_r, MIN_MU_R)
        vr, v0 = max(self.var_r, 0.25), max(self.var_0, 0.25)
        ll_real = -0.5 * np.log(vr) - 0.5 * (z - mu) ** 2 / vr
        ll_fa = -0.5 * np.log(v0) - 0.5 * z ** 2 / v0
        lr_p = np.log(self.pc_real[pers] / self.pc_real.sum()) - np.log(self.pc_fa[pers] / self.pc_fa.sum())
        logit = (ll_real - ll_fa) + lr_p + math.log(max(self.prior, 1e-3) / max(1 - self.prior, 1e-3))
        p_real = 1.0 / (1.0 + np.exp(-np.clip(logit, -30, 30)))

        # --- online updates (use this CPI's own posteriors as soft labels)
        w = p_real
        if w.sum() > 0.5 and (1 - w).sum() > 0.5:
            mu_obs = float((w * z).sum() / w.sum())
            self.mu_r = (1 - EMA) * self.mu_r + EMA * max(mu_obs, MIN_MU_R)
            vr_obs = float((w * (z - mu_obs) ** 2).sum() / w.sum())
            v0_obs = float(((1 - w) * z ** 2).sum() / max((1 - w).sum(), 1e-9))
            self.var_0 = (1 - EMA) * self.var_0 + EMA * max(v0_obs, 0.25)
            # var_r >= var_0 is an IDENTIFIABILITY constraint, not a tuned value. Real targets vary in
            # RCS, range and aspect, so their SNR spread is necessarily at least as wide as the noise
            # class -- and without the constraint the EM has a degenerate fixed point: a narrow var_r
            # makes only the strongest detections "real", whose variance is then small, which keeps
            # var_r narrow. Measured collapsing to 0.26 against a true real-class variance of 2.51,
            # which is what was throwing away every weak-but-genuine detection.
            self.var_r = (1 - EMA) * self.var_r + EMA * max(vr_obs, self.var_0)
            self.prior = float(np.clip((1 - EMA) * self.prior + EMA * w.mean(), 0.02, 0.95))
            for i in range(n):
                self.pc_real[pers[i]] += w[i]
                self.pc_fa[pers[i]] += 1.0 - w[i]

        self.recent.append(list(zip(rbins, dbins)))
        self.recent = self.recent[-PERSIST_WINDOW:]
        return p_real


def main():
    # Reuse the labelling machinery so this is scored identically to every other result.
    exec(open("analyze_separability.py").read().split("def main()")[0], globals())
    for run_dir in sys.argv[1:]:
        run_dir = run_dir.rstrip("/")
        reports = [json.loads(l) for l in open(f"{run_dir}/oaiue_reports.jsonl") if l.strip()]
        curves, tf = load_curves(f"{run_dir}/logs/ue.log")          # noqa: F821
        epochs = cpi_epochs(reports, curves, 15.0, 3.0, tf, 0.6)     # noqa: F821

        q = AdaptiveDetQuality()
        keep_lab, drop_lab = [], []
        for idx, rep in enumerate(reports):
            dets = rep["detections"]
            rres = rep.get("range_res_m") or 1.0
            vres = rep.get("vel_res_mps") or 1.0
            snrs = [d["snr_db"] for d in dets]
            rb = [d["bistatic_range_m"] / rres for d in dets]
            db = [d["bistatic_velocity_mps"] / vres for d in dets]
            dt = (rep.get("cpi_duration_ns") or 0) * 1e-9
            drift = [d["bistatic_velocity_mps"] * dt / rres for d in dets]
            p = q.score(snrs, rb, db, drift)
            for i, d in enumerate(dets):
                lab = label(d, epochs[idx], curves, 15.0, 3.0, tf)    # noqa: F821
                (keep_lab if p[i] >= q.boundary else drop_lab).append(lab)

        kr = sum(1 for l in keep_lab if l == "real")
        dr = sum(1 for l in drop_lab if l == "real")
        tot_real = kr + dr
        n = len(keep_lab) + len(drop_lab)
        print(f"=== {run_dir}")
        print(f"  detections {n} -> kept {len(keep_lab)} ({100*len(keep_lab)/n:.0f} %)")
        print(f"  precision  {100*sum(1 for l in keep_lab if l=='real')/max(len(keep_lab),1):5.1f} % "
              f"(was {100*tot_real/n:5.1f} %)")
        print(f"  recall of real detections {100*kr/max(tot_real,1):5.1f} %  "
              f"(lost {dr} of {tot_real})")
        for cls in ("harmonic", "other"):
            tot = sum(1 for l in keep_lab + drop_lab if l == cls)
            kept = sum(1 for l in keep_lab if l == cls)
            if tot:
                print(f"  {cls:<9} kept {kept:4d}/{tot:<4d} ({100*kept/tot:5.1f} % survive)")
        print(f"  learned state: mu_r={q.mu_r:.2f} var_r={q.var_r:.2f} var_0={q.var_0:.2f} prior={q.prior:.2f}, "
              f"null med={q.med:.1f} dB mad={q.mad:.2f} dB")
    return 0


if __name__ == "__main__":
    sys.exit(main())
