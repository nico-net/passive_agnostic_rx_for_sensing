#!/usr/bin/env python3
# SPDX-License-Identifier: LicenseRef-CSSL-1.0
"""Offline planner mirroring common/utils/nr_cell_admit.c (task B3) for what-if tables.

It reuses no process state and probes nothing: the host is described by the same fields as nr_host_caps_t. The arithmetic is integer and follows the C function
step by step (nr_host_membudget.h / nr_gpu_membudget.h pure functions, the receiver's per-module sizing expressions), so
`test_admit_model.py` can compare it with the C function on the fixtures of admit_fixtures.txt, line by line.

  admit_model.py FIXTURE            print the plan of every case in the C CLI's output format
  admit_model.py --table            what-if table: cells of one grid on the three fixture hosts (sens6 / dgx / cloud4; fixtures, not inputs of the model)

Every constant is labelled in nr_cell_admit.c with its source ([CODE file:line], [MEASURED report], [HYPOTHESIS task]); it is repeated here only to mirror it.
Evidence: a model estimate [CODE-READ], not a measurement, until G1 supplies measured per-cell bytes."""
import sys

MIB = 1 << 20
SIZE_MAX = (1 << 64) - 1
NR_CELL_MAX = 16
PLAN_MAX = 64
OK, CORES, RAM, GPU_MEM, CHANNELIZER, MAX, LOST = range(7)
REASON = ["none", "cores", "ram", "gpu_mem", "channelizer", "max", "lost", "pool"]
NEW, ACQUIRE, MAINTAIN, LOSTST = range(4)
R_SNAP, R_CSIRS, R_TD, R_GW, R_GSLOTS = 1, 2, 4, 8, 16

# ---- pure functions of nr_host_membudget.h ----
HOST_RESERVE = 1536 * MIB
HOST_HARD = 512 * MIB


def host_calc(avail):
    if avail == SIZE_MAX:
        return SIZE_MAX
    usable = avail // 5 * 4
    return usable - HOST_RESERVE if usable > HOST_RESERVE else 0


def fit(share, fixed, per_unit, lo, hi):
    if share == SIZE_MAX or fixed + per_unit * hi <= share:
        return hi
    if share <= fixed or not per_unit:
        return 0
    k = (share - fixed) // per_unit
    return 0 if k < lo else k


def fit_pct(headroom, own, pct, fixed, per_unit, lo, hi):
    if headroom == SIZE_MAX:
        return hi
    room = (headroom // 100) * pct + own
    return fit(room, fixed, per_unit, lo, hi)


# ---- pure functions of nr_gpu_membudget.h ----
GPU_RESERVE = 768 * MIB
GPU_WEIGHT = {"csirs": 36, "ldpc_pool": 8, "ldpc_cb0": 11, "td_cb0": 7, "nid": 10, "pass": 8, "graph": 20}


def gpu_calc(free):
    if not free:
        return SIZE_MAX
    usable = free // 20 * 17
    return usable - GPU_RESERVE if usable > GPU_RESERVE else 0


def share_of(budget, m):
    return SIZE_MAX if budget == SIZE_MAX else budget // 100 * GPU_WEIGHT[m]


def sat_sub(a, b):
    if a == SIZE_MAX:
        return a
    return 0 if b >= a else a - b


def split(x, w, W):
    if x == SIZE_MAX:
        return SIZE_MAX
    return x if w == W else x // W * w


# ---- geometry and module sizes (see nr_cell_admit.c for the sources) ----
def max_grid(scs):
    return 270 if scs == 15 else 135 if scs == 60 else 264 if scs == 120 else 273


class Geom:
    def __init__(self, c):
        self.scs = c.get("scs", 30) or 30
        self.n_rb = c.get("rb", 0) or max_grid(self.scs)
        rx = c.get("rx", 0)
        self.rx = (4 if rx > 4 else rx) if rx > 0 else 4
        self.mu = {15: 0, 30: 1, 60: 2}.get(self.scs, 3)
        f = 128
        while f <= self.n_rb * 12:
            f <<= 1
        self.fft = f
        self.fs = f * self.scs * 1000
        self.spf = self.fs // 100
        self.rxdf = 14 * f
        self.stride = 14 * (f + f // 128 * 9 + f // 128 * (9 + (1 << self.mu)))
        wc = c.get("wcentre", 0)
        self.shifted = not (wc != 0 and c.get("centre", 0) == wc)


def iq_set(g):
    return 2 * g.spf * 2 * 2 * g.rx


def h_queue(g, k):
    cons = 1 if k["consumers"] < 1 else k["consumers"]
    return cons * (g.rx * g.rxdf * 4 + (iq_set(g) if k["csirs"] else 0))


def h_csirs_slot(g):
    return g.rx * 14 * 24 * g.n_rb * 2


def h_td_bytes(g, cap):
    slot = (g.stride * g.rx * 4 + 63) // 64 * 64
    huge = 2 * MIB
    pool = (slot * (cap + 100) + huge - 1) // huge * huge
    return pool + 2 * 100 * g.rx * 14 * g.fft * 4


def h_gw_unit(g):
    c, bytes_, k = 4096, g.n_rb * 12 * 14 * 4 * 8 * 2, 0
    while c < bytes_ and k < 23:
        c <<= 1
        k += 1
    return c + 64 if c >= bytes_ else 0


def _cons(k):
    return 1 if k["consumers"] < 1 else 6 if k["consumers"] > 6 else k["consumers"]


def h_gw_full(k):
    if not k["gw"]:
        return 0
    cons = _cons(k)
    depth = k["depth"] if k["depth"] > 0 else 8 * cons
    depth = max(2, min(64, depth))
    return cons * depth * 16


def h_gw_floor(k):
    return min(_cons(k) * 8, h_gw_full(k))


def csirs_mem_fixed(rb, ant, fft, stride, batch):
    tiles = (rb + 15) // 16
    pooled = 168 * tiles
    nr = 14 * 12 * rb + pooled
    b = 640 * nr * 8 + 640 * pooled * 8 + 80 * nr * 8 + 64 * nr * 8 + 2 * nr * 8 + 2 * 640 * pooled * 8 + 640 * 8
    b += 100 * nr * 4 + 100 * ant * 14 * 24 * rb * 2
    b += 14 * 12 * rb + 640 * 14 * tiles * 6 * 2
    b += batch * ant * 14 * fft * 8 + batch * ant * stride * 4
    b += 1024 * 160 * 4 + rb * 16 * 8 * 2 * 4
    b += 2048 * (39 * rb + (rb + 7) // 8 + 1) * 8
    b += 512 << 20
    return b


def csirs_mem_per_slot(rb, ant):
    return rb * 32 * 8 * ant * 4 + ant * 14 * 12 * rb * 4 + 1024 * 160 * 4 + ant * 14 * 24 * rb * 2 + 64


CSIRS_LOCAL = 512 * MIB
HARQ = 2048 * 66 * 384 * 2
NID_BATCH, NID_FULL, NID_FLOOR, NID_DUR = 336, 10, 5, 3
NID_STAT = 6 * 14 * 65536
# G1: the nID context bytes are the expressions of openair1/PHY/NR_UE_TRANSPORT/nr_gpu_cell.h (nr_pdcch_nid_gpu.cu window_bytes / fixed_bytes)
NID_SNAP, NID_HYP, NID_SUMMARY, NID_DEVICE, NID_IDS, NID_CHUNK, NID_HYP_CAP = 56, 56, 12, 8425776, 65536, 4, 1 << 20
GRAPH_FLOOR = 512 * MIB


def g_csirs_fixed(g):
    f = csirs_mem_fixed(g.n_rb, g.rx, g.fft, g.stride, 100)
    return f - CSIRS_LOCAL if f > CSIRS_LOCAL else 0


def g_nid_win(rb, window):
    n, sc = window * NID_BATCH, 12 * rb
    return n * NID_DUR * sc * 4 + 2 * n * 4 + n * 4 * 2 + (4 * n + 1) * NID_SNAP + ((4 * n + 1) * 6 * 276 + 2 * n) * 8 + window * NID_STAT * (2 + 4)


def nid_fixed_bytes():
    cells, chunk_ids = 14 * NID_IDS, NID_CHUNK * NID_IDS
    return (NID_DEVICE + chunk_ids * 4 + 2 * chunk_ids * 4 + 4 * chunk_ids * 4 + 6 * cells * (4 + 2) + NID_STAT * (2 + 8)
            + NID_STAT // 256 * NID_SUMMARY + NID_HYP_CAP * NID_HYP)


def g_nid_batch(rb):
    return g_nid_win(rb, 2) - g_nid_win(rb, 1)


def g_nid_fixed():
    one, per = g_nid_win(273, 1), g_nid_batch(273)
    return nid_fixed_bytes() + (one - per if one > per else 0)


def g_graph_cap(free):
    q = free // 4 if free else 3072 * MIB
    return max(2048 * MIB, min(3072 * MIB, q))


class Eval:
    pass


def eval_gpu(k, G, w, S, e):
    e.gB, e.gused, e.gshared, e.graph, e.nid, e.gtotal_ok = SIZE_MAX, 0, 0, 0, 0, True
    if k["tier"] == 0 or not S:
        return
    B = gpu_calc(k["gpu_free"])
    e.gB = B
    W = sum(w[i] for i in S)
    rbmax = max(G[i].n_rb for i in S)
    if k["gpu_shared"]:
        shared = k["gpu_shared"]
    else:
        pass_inst = 361 * MIB - HARQ
        shared = (min(620 * MIB, share_of(B, "ldpc_pool")) + min(874 * MIB, share_of(B, "ldpc_cb0"))
                  + min(556 * MIB, share_of(B, "td_cb0")) + min(pass_inst, share_of(B, "pass")))
    extra = 0
    if len(S) > 1 and B != SIZE_MAX:
        sg = share_of(B, "graph")
        extra = sg - GRAPH_FLOOR if sg > GRAPH_FLOOR else 0
    wc, wn = GPU_WEIGHT["csirs"], GPU_WEIGHT["nid"]
    csirs_pool = SIZE_MAX if B == SIZE_MAX else share_of(B, "csirs") + extra // (wc + wn) * wc
    nid_pool = SIZE_MAX if B == SIZE_MAX else share_of(B, "nid") + extra // (wc + wn) * wn
    used = shared
    if k["csirs"]:
        sumfix = sum(g_csirs_fixed(G[i]) for i in S)
        need = CSIRS_LOCAL + sumfix
        left = SIZE_MAX if csirs_pool == SIZE_MAX else (csirs_pool - need if csirs_pool > need else 0)
        anyc = False
        for i in S:
            g = G[i]
            fx = g_csirs_fixed(g)
            sh = SIZE_MAX if left == SIZE_MAX else fx + split(left, w[i], W)
            s = fit(sh, fx, csirs_mem_per_slot(g.n_rb, g.rx), 160, 640)
            e.c[i]["gslots"] = s
            if s:
                e.c[i]["gbytes"] += fx + s * csirs_mem_per_slot(g.n_rb, g.rx)
                anyc = True
        if anyc:
            used += CSIRS_LOCAL
    for i in S:
        e.c[i]["gbytes"] += HARQ
        used += e.c[i]["gbytes"]
    wv = fit(nid_pool, g_nid_fixed(), g_nid_batch(rbmax), NID_FLOOR, NID_FULL)
    e.nid = wv
    if wv:
        used += g_nid_fixed() + wv * g_nid_batch(rbmax)
    e.gshared, e.gused = shared, used
    cap = g_graph_cap(k["gpu_free"])
    e.graph = cap if B == SIZE_MAX else (min(cap, B - used) if B > used else 0)
    e.gtotal_ok = B == SIZE_MAX or used + GRAPH_FLOOR <= B


def eval_host(k, G, w, S, e):
    B = host_calc(k["mem_avail"])
    H = SIZE_MAX if k["mem_avail"] == SIZE_MAX else sat_sub(k["mem_avail"], HOST_HARD)
    if k["unified"] and S:
        gpu = e.gused + (0 if e.gB == SIZE_MAX else e.graph)
        if B != SIZE_MAX:
            B = sat_sub(B, gpu)
        if H != SIZE_MAX:
            H = sat_sub(H, gpu)
    e.hB, e.hH = B, H
    Bs = sat_sub(B, k["shared"])
    Ftot = sum(h_queue(G[i], k) for i in S)
    W = sum(w[i] for i in S)
    left = SIZE_MAX if Bs == SIZE_MAX else sat_sub(Bs, Ftot)
    total = k["shared"]
    for i in S:
        g, r = G[i], e.c[i]
        q = h_queue(g, k)
        A = SIZE_MAX if left == SIZE_MAX else q + split(left, w[i], W)
        hr, nbytes, fixedc = sat_sub(A, q), q, q
        if k["recovery"] and k["csirs"]:
            s = fit_pct(hr, 0, 10, 0, iq_set(g), 2, 8)
            r["snap"] = s
            if s:
                b = s * iq_set(g)
                nbytes += b
                fixedc += b
                hr = sat_sub(hr, b)
        if k["csirs"]:
            s = fit_pct(hr, 0, 30, 0, h_csirs_slot(g), 160, 640)
            r["csirs"] = s
            if s:
                b = s * h_csirs_slot(g)
                nbytes += b
                fixedc += b
                hr = sat_sub(hr, b)
                b1 = h_td_bytes(g, 1)
                per = (h_td_bytes(g, 1025) - b1) // 1024
                f0 = b1 - per if b1 > per else 0
                t = fit_pct(hr, 0, 60, f0 + 2 * MIB, per, 256, 1024)
                r["td"] = t
                if t:
                    tb = h_td_bytes(g, t)
                    nbytes += tb
                    fixedc += tb
        full = h_gw_full(k)
        if full:
            share = SIZE_MAX if A == SIZE_MAX else sat_sub(A, fixedc) // 100 * 60
            f = fit(share, 0, h_gw_unit(g), h_gw_floor(k), full)
            r["gw"] = f
            nbytes += f * h_gw_unit(g)
        r["hbytes"] = nbytes
        total += nbytes
        r["ready"] = ((R_SNAP if r["snap"] else 0) | (R_CSIRS if r["csirs"] else 0) | (R_TD if r["td"] else 0)
                      | (R_GW if r["gw"] else 0) | (R_GSLOTS if r["gslots"] else 0))
    e.hused = total
    e.htotal_ok = H == SIZE_MAX or total <= H


def new_eval():
    e = Eval()
    e.c = [dict(snap=0, csirs=0, td=0, gw=0, gslots=0, hbytes=0, gbytes=0, ready=0) for _ in range(PLAN_MAX)]
    e.nid = 0
    e.hB = e.hH = e.hused = e.gB = e.gused = e.gshared = e.graph = 0
    e.gtotal_ok = e.htotal_ok = True
    return e


def eval_set(k, G, w, S):
    e = new_eval()
    eval_gpu(k, G, w, S, e)
    eval_host(k, G, w, S, e)
    return e


def required(k):
    m = 0
    if k["csirs"]:
        m |= R_CSIRS | R_TD
        if k["recovery"]:
            m |= R_SNAP
        if k["tier"] != 0:
            m |= R_GSLOTS
    if k["gw"]:
        m |= R_GW
    return m


def default_caps():
    return dict(mem_avail=SIZE_MAX, shared=0, tier=0, unified=0, gpu_free=0, gpu_shared=0, max_cells=-1, chan=-1.0, consumers=4, depth=64, csirs=0,
                recovery=0, gw=0)


def plan(k, cells):
    n = len(cells)
    if n > PLAN_MAX:
        return None
    G = [Geom(c) for c in cells]
    w = [c.get("weight", 1) if c.get("weight", 1) > 0 else 1 for c in cells]
    st = [c.get("state", NEW) for c in cells]
    out = dict(cell=[dict(served=0, reason=OK, order=-1, snap=0, csirs=0, td=0, gw=0, gslots=0, host=0, gpu=0) for _ in range(n)], served=0, refuse=0,
               binding=OK, hused=0, gused=0, nid=0, graph=0)
    primary = next((i for i in range(n) if st[i] != LOSTST), -1)
    order = [primary] if primary >= 0 else []
    for cls in (MAINTAIN, ACQUIRE, NEW):
        order += [i for i in range(n) if i != primary and st[i] == cls]
    for i in range(n):
        if st[i] == LOSTST:
            out["cell"][i]["reason"] = LOST
    S = []
    cur = new_eval()
    chan = 0.0
    req = required(k)
    for oi, ci in enumerate(order):
        reason = OK
        out["cell"][ci]["order"] = oi
        nxt = eval_set(k, G, w, S + [ci])
        if oi == 0:
            if not nxt.htotal_ok:
                out["refuse"] = 1
                out["cell"][ci]["reason"] = RAM
                out["hused"] = nxt.hused
                for oj in range(1, len(order)):
                    out["cell"][order[oj]]["reason"] = RAM
                    out["cell"][order[oj]]["order"] = oj
                out["binding"] = RAM
                return out
        else:
            ns = len(S)
            if ns >= NR_CELL_MAX:
                reason = MAX
            elif k["max_cells"] != -1 and ns >= max(1, k["max_cells"]):
                reason = CORES
            elif G[ci].shifted and k["chan"] >= 0 and chan + G[ci].fs / 1e6 > k["chan"] + 1e-9:
                reason = CHANNELIZER
            else:
                gok = nxt.gtotal_ok and (not (req & R_GSLOTS) or nxt.c[ci]["gslots"]) and (cur.nid == 0 or nxt.nid)
                for i in S:
                    if gok and (cur.c[i]["ready"] & R_GSLOTS) and not nxt.c[i]["gslots"]:
                        gok = False
                if not gok and k["tier"] != 0:
                    reason = GPU_MEM
                else:
                    mask = req & ~R_GSLOTS
                    hok = nxt.htotal_ok and (nxt.c[ci]["ready"] & mask) == mask
                    for i in S:
                        had = cur.c[i]["ready"] & ~R_GSLOTS
                        if hok and (nxt.c[i]["ready"] & had) != had:
                            hok = False
                    if not hok:
                        reason = RAM
        if reason != OK:
            out["cell"][ci]["reason"] = reason
            if not out["binding"]:
                out["binding"] = reason
            continue
        S.append(ci)
        cur = nxt
        if G[ci].shifted:
            chan += G[ci].fs / 1e6
        out["cell"][ci]["served"] = 1
        out["cell"][ci]["reason"] = OK
    out["served"] = len(S)
    if S:
        for i in S:
            r, o = cur.c[i], out["cell"][i]
            o.update(snap=r["snap"], csirs=r["csirs"], td=r["td"], gw=r["gw"], gslots=r["gslots"], host=r["hbytes"], gpu=r["gbytes"])
        out["hused"] = 0 if cur.hused == SIZE_MAX else cur.hused
        out["gused"] = 0 if cur.gB == SIZE_MAX and not cur.gused else cur.gused
        out["nid"], out["graph"] = cur.nid, cur.graph
    return out


# ---- fixtures / text format shared with the C CLI ----
def parse_fixture(text):
    cases, k, cells, name = [], None, None, None
    for line in text.splitlines():
        tok = line.split()
        if not tok or tok[0].startswith("#"):
            continue
        if tok[0] == "case":
            name, k, cells = tok[1], default_caps(), []
        elif tok[0] == "caps":
            for t in tok[1:]:
                key, v = t.split("=")
                v = float(v)
                if key == "mem_avail_mb":
                    k["mem_avail"] = SIZE_MAX if v < 0 else int(v) * MIB
                elif key == "shared_mb":
                    k["shared"] = int(v) * MIB
                elif key == "tier":
                    k["tier"] = int(v)
                elif key == "unified":
                    k["unified"] = int(v)
                elif key == "gpu_free_mb":
                    k["gpu_free"] = int(v) * MIB
                elif key == "gpu_shared_mb":
                    k["gpu_shared"] = int(v) * MIB
                elif key == "max_cells":
                    k["max_cells"] = int(v)
                elif key == "chan_msps":
                    k["chan"] = v
                elif key in ("consumers", "depth", "csirs", "recovery", "gw"):
                    k[key] = int(v)
        elif tok[0] == "cell":
            c = dict(scs=30)
            for t in tok[1:]:
                key, v = t.split("=")
                c[key] = float(v) if key in ("centre", "wcentre") else int(float(v))
            cells.append(c)
        elif tok[0] == "end":
            cases.append((name, k, cells))
    return cases


def format_case(name, k, cells):
    p = plan(k, cells)
    lines = []
    for i, c in enumerate(p["cell"]):
        lines.append(f"{name} {i} served={c['served']} reason={REASON[c['reason']]} order={c['order']} snap={c['snap']} csirs={c['csirs']} td={c['td']} gw={c['gw']} "
                     f"gslots={c['gslots']} host={c['host']} gpu={c['gpu']}")
    lines.append(f"{name} plan served={p['served']} refuse={p['refuse']} binding={REASON[p['binding']]} hused={p['hused']} gused={p['gused']} nid={p['nid']} graph={p['graph']}")
    return lines


HOSTS = {  # synthetic fixture hosts (labelled in admit_fixtures.txt); NOT inputs of the model
    "sens6": dict(mem_avail=9216 * MIB, shared=1019 * MIB, tier=2, gpu_free=7800 * MIB, max_cells=-1),
    "dgx": dict(mem_avail=102400 * MIB, shared=1100 * MIB, tier=1, unified=1, gpu_free=102400 * MIB, max_cells=4),
    "cloud4": dict(mem_avail=16384 * MIB, shared=0, tier=0, max_cells=1),
}


def table():
    print("what-if: served cells of one grid (4 RX, 30 kHz) on the fixture hosts [CODE-READ model estimate, not a measurement]")
    print(f"{'host':8} {'PRB':>4} {'n=1':>4} {'n=2':>4} {'n=3':>4} {'n=4':>4} {'n=6':>4}  binding at n=6")
    for h, over in HOSTS.items():
        for rb in (51, 106, 217, 273):
            row, bind = [], ""
            for n in (1, 2, 3, 4, 6):
                k = default_caps()
                k.update(over, csirs=1, recovery=1, gw=1)
                p = plan(k, [dict(rb=rb, scs=30, rx=4) for _ in range(n)])
                row.append(p["served"])
                bind = REASON[p["binding"]]
            print(f"{h:8} {rb:>4} " + " ".join(f"{x:>4}" for x in row) + f"  {bind}")


if __name__ == "__main__":
    if len(sys.argv) > 1 and sys.argv[1] == "--table":
        table()
    elif len(sys.argv) > 1:
        for name, k, cells in parse_fixture(open(sys.argv[1]).read()):
            print("\n".join(format_case(name, k, cells)))
    else:
        print(__doc__)
