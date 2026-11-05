"""Shared helpers for CSIRS-CLASS (offline, raw 4-RX capture). Reuses tools/csirs_capture_map.py."""
import json, os, sys
import numpy as np
sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))
import csirs_capture_map as M

NRB, NSC = M.NRB, M.NSC


def open_slotter(cap_dir, coh_npz, sym_shift=1):
    """Slotter whose slot j starts at the TRUE slot boundary. map_coh.py's grid is slipped by `sym_shift` symbols
    (my l = true l - 1 for the DM-RS/PDCCH evidence, see report), so move t0 back by that many symbols."""
    meta = json.loads(str(np.load(coh_npz)["meta"]))
    cap = M.Cap(cap_dir, 4)
    t0 = (meta["t0"] - sum(M.CP_LEN[l] + M.NFFT for l in range(sym_shift))) % M.SLOT if sym_shift else meta["t0"]
    # symbol 0 has the long CP: moving back by one symbol is (CP0+NFFT); for sym_shift>1 add normal symbols
    if sym_shift > 1:
        t0 = (meta["t0"] - (M.CP0 + M.NFFT) - (sym_shift - 1) * (M.CP + M.NFFT)) % M.SLOT
    return M.Slotter(cap, t0, meta["cfo"], meta["drift"]), meta


def seq(ids, ns, l, nre):
    return M.csirs_seq(ids, ns, l, nre)


def group_geometry(row, k_bits_sc, l0, l1):
    """OAI nr_phy_common_csirs.c get_csi_mapping_parms for the 32-port rows used here. Returns list of
    (group j, symbol, k_bar) and (gs, kprime, lprime). k_bits_sc = list of the 4 k_bar (subcarrier of each fd pair in the RB)."""
    if row == 16:
        return [(i, (l0 + (i >> 2)) if i < 8 else (l1 + ((i - 8) >> 2)), k_bits_sc[i % 4]) for i in range(16)], 2, 1, 0
    if row == 17:
        return [(i, l0 if i < 4 else l1, k_bits_sc[i % 4]) for i in range(8)], 4, 1, 1
    raise ValueError(row)


def wf_wt(s, kp, lp):
    wf = 1 if kp == 0 else (-2 * (s % 2) + 1)
    if s < 2: wt = 1
    elif s < 4: wt = -2 * (lp % 2) + 1
    elif s < 6: wt = -2 * (lp // 2) + 1
    else: wt = 1 if lp in (0, 3) else -1
    return wf, wt


def despread(Ys, ns, ids, row, kbars, l0, l1, rho=1.0, conv='A'):
    """Ys: dict true symbol -> (4, NSC) frequency-domain data of the slot. Returns H (nrb, 32 ports, 4 rx) = per-RB port channel
    (density 1, alpha = 2*rho as in OAI), no scaling. Port p = s + j*gs."""
    groups, gs, kpmax, lpmax = group_geometry(row, kbars, l0, l1)
    rcache = {}
    def r(l):
        if l not in rcache: rcache[l] = seq(ids, ns, l, 2 * NRB + 2)[0]
        return rcache[l]
    H = np.zeros((NRB, 32, 4), np.complex64)
    for (j, lb, kb) in groups:
        for s in range(gs):
            acc = np.zeros((NRB, 4), np.complex64)
            nrm = 0
            for kp in range(kpmax + 1):
                for lp in range(lpmax + 1):
                    wf, wt = wf_wt(s, kp, lp)
                    l = lb + lp
                    ks = np.arange(NRB) * 12 + kb + kp
                    m = np.arange(NRB) * int(round(2 * rho)) + kp + int(rho * kb) // 12
                    y = Ys[l][:, ks]  # (4, nrb)
                    acc += (y * np.conj(wf * wt * r(l)[m])[None, :]).T
                    nrm += 1
            H[:, (s + j * gs) if conv == 'A' else (j + s * len(groups)), :] = acc / nrm
    return H


def despread_groups(Ys, ns, ids, symbols, kbars, rho=1.0, conv='A'):
    """Generic fd-CDM2 despread (row-agnostic): groups ordered symbol-major then k ascending; port p = s + 2j (conv A) or j + s*ngroups (B)."""
    groups = [(l, kb) for l in symbols for kb in kbars]
    H = np.zeros((NRB, 2 * len(groups), 4), np.complex64)
    for j, (l, kb) in enumerate(groups):
        r = seq(ids, ns, l, 2 * NRB + 2)[0]
        for s in range(2):
            acc = 0
            for kp in range(2):
                wf = 1 if kp == 0 else (-2 * (s % 2) + 1)
                ks = np.arange(NRB) * 12 + kb + kp
                acc = acc + (Ys[l][:, ks] * np.conj(wf * r[np.arange(NRB) * 2 + kp])[None, :]).T
            H[:, (s + 2 * j) if conv == 'A' else (j + s * len(groups)), :] = acc / 2
    return H
