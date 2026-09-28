#!/usr/bin/env python3
"""ZMQ sample broker: OCUDU gNB <-> srsUE (active) + OAI --passive-rx receiver (listen-only).

All three ends speak the same srsRAN-style ZMQ radio protocol (OAI's radio/zmq is a port of it):
cf32 samples, the TRANSMITTER binds a REP socket, the RECEIVER connects a REQ socket, sends a 1-byte
request and gets back whatever samples the transmitter has queued. There is no timestamp on the
wire: time == sample index, so every lane must be forwarded 1:1, never dropped or duplicated.

Lanes (default ports):
  gNB TX  REP :2000  --(REQ)-->  broker  --(REP :2100)--> srsUE RX      DL, forwarded verbatim
  srsUE TX REP :2001 --(REQ)-->  broker  --(REP :2101)--> gNB RX        UL, forwarded verbatim
                                 broker  --(REP :2200)--> passive RX    DL[k] + g*UL[k]
  passive TX REP :2201 --(REQ)-> broker (sink)                          zeros, discarded

Why DL+UL summed is right: TDD, so at any sample index at most one of the two carries signal, and
gNB-RX index k == srsUE-TX index k by construction (1:1 forwarding), i.e. the passive sees an ideal
co-located, zero-propagation observer of both links.

Pacing / lockstep behaviour (documented because it is the whole point of a broker):
  * gNB <-> srsUE is a closed loop: the gNB only produces DL as it receives UL, srsUE only produces UL
    as it receives DL. The broker never breaks it; if srsUE stalls, the gNB stalls (same as direct).
  * The passive is OPEN loop (its TX is a sink). OAI's RX poll thread requests continuously and
    pushes into a 1 s ring that silently overflows, so the broker paces it: OAI's TX sink emits
    exactly as many (zero) samples as its RX path has consumed (zmq_rx_stream::receive -> tx align),
    so sink bytes are a consumption clock. The broker never lets (sent - consumed) exceed --px-window.
  * Backpressure: while the passive is live, the broker stops pulling DL from the gNB once the
    passive falls --px-hwm behind. The whole cell then runs at the passive's speed (no samples lost,
    nothing is real-time here anyway).
  * A passive that makes no progress for --px-stall seconds while data is waiting is declared dead:
    its lane is dropped and the gNB/srsUE loop continues unthrottled. If it (or a restarted
    instance) requests again, the lane is re-opened at the current DL index (logged as a
    discontinuity). So restarting ONLY the passive is supported; gNB/srsUE still restart together.
  * Until srsUE's first DL request the gNB's UL is synthesized as zeros up to the DL index and the
    gNB's DL is not queued for srsUE, so the cell (and the passive) run with no UE attached. At srsUE's
    first request the UL is padded to the current DL index and srsUE's stream starts there: srsUE
    TX index j then lands on gNB RX index D0+j, the same relation a direct link has from index 0.
    This is what lets the passive sync BEFORE the UE attaches (PX_FIRST) and see PRACH/RAR/Msg4.
  * --no-ue: never switch to srsUE (gNB + passive only).
"""
import argparse
import time

import numpy as np
import zmq

CF = 8  # bytes per cf32 sample
OAI_MAX_MSG = 300000       # OAI radio/zmq rx_buffer_size (samples)
OCUDU_MAX_MSG = 614400     # OCUDU DEFAULT_STREAM_BUFFER_SIZE (samples)
SRSUE_MAX_MSG = 3072000    # srsRAN rf_zmq ZMQ_MAX_BUFFER_SIZE (samples)


class Q:
    """FIFO of byte frames with a sample count."""

    def __init__(self):
        self.frames, self.n = [], 0

    def put(self, b):
        self.frames.append(b)
        self.n += len(b) // CF

    def take(self, max_samp):
        out, got = [], 0
        while self.frames and got < max_samp:
            f = self.frames[0]
            k = min(len(f) // CF, max_samp - got)
            if k * CF == len(f):
                out.append(self.frames.pop(0))
            else:
                out.append(f[:k * CF])
                self.frames[0] = f[k * CF:]
            got += k
        self.n -= got
        return out[0] if len(out) == 1 else b''.join(out)

    def clear(self):
        self.frames, self.n = [], 0


def rms_db(a):
    return 10 * np.log10(np.mean(np.abs(a) ** 2) + 1e-30) if len(a) else float('-inf')


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--gnb-tx', default='tcp://127.0.0.1:2000')
    ap.add_argument('--gnb-rx', default='tcp://127.0.0.1:2101')
    ap.add_argument('--ue-tx', default='tcp://127.0.0.1:2001')
    ap.add_argument('--ue-rx', default='tcp://127.0.0.1:2100')
    ap.add_argument('--px-rx', default='tcp://127.0.0.1:2200')
    ap.add_argument('--px-tx', default='tcp://127.0.0.1:2201')
    ap.add_argument('--srate', type=float, default=23.04e6)
    ap.add_argument('--no-ue', action='store_true')
    ap.add_argument('--px-window', type=float, default=0.1, help='max unconsumed samples at the passive [s]')
    ap.add_argument('--px-hwm', type=float, default=0.3, help='passive lag that throttles the gNB [s]')
    ap.add_argument('--px-stall', type=float, default=30.0, help='no-progress time before dropping the passive [s]')
    ap.add_argument('--px-gain-db', type=float, default=-6.0,
                    help='gain on the passive lane (OAI maps +-1.0 to int16 full scale; OCUDU DL peaks near -12 dBFS)')
    ap.add_argument('--px-ul-gain-db', type=float, default=0.0,
                    help='UL level relative to DL inside the passive lane. UL is first auto-normalised to the DL level '
                         '(srsUE ZMQ TX comes out ~+47 dBFS, it would rail OAI int16)')
    ap.add_argument('--no-ul-norm', action='store_true', help='do not normalise UL to the DL level (self-test)')
    ap.add_argument('--stats', type=float, default=5.0)
    a = ap.parse_args()

    ctx = zmq.Context()

    def sock(kind, addr, bind):
        s = ctx.socket(kind)
        s.setsockopt(zmq.LINGER, 0)
        if kind == zmq.REQ:
            s.setsockopt(zmq.REQ_RELAXED, 1)
            s.setsockopt(zmq.REQ_CORRELATE, 1)
        (s.bind if bind else s.connect)(addr)
        return s

    gnb_dl = sock(zmq.REQ, a.gnb_tx, False)
    gnb_ul = sock(zmq.REP, a.gnb_rx, True)
    ue_dl = sock(zmq.REP, a.ue_rx, True)
    ue_ul = None if a.no_ue else sock(zmq.REQ, a.ue_tx, False)
    px_dl = sock(zmq.REP, a.px_rx, True)
    px_tx = sock(zmq.REQ, a.px_tx, False)

    win, hwm = int(a.px_window * a.srate), int(a.px_hwm * a.srate)
    hwm_ue = 1500000
    g_px = np.float32(10 ** (a.px_gain_db / 20))
    g_ul_rel = 10 ** ((a.px_gain_db + a.px_ul_gain_db) / 20)
    pmax = {'dl': 0.0, 'ul': 0.0}  # max block power seen so far (linear) -> UL auto-normalisation

    ue_q, ul_q = Q(), Q()
    ue_on = False
    dl_total = ul_total = 0
    req_out = {'gnb_dl': False, 'ue_ul': False, 'px_tx': False}
    owed = {'gnb_ul': False, 'ue_dl': False, 'px_dl': False}
    # passive lane state
    px = dict(on=False, start=0, sent=0, cons=0, skip_dl=0, skip_ul=0, last=0.0, opens=0)
    px_dlb = np.zeros(0, np.complex64)
    px_ulb = np.zeros(0, np.complex64)
    px_out = np.zeros(0, np.complex64)
    pk = {'dl': float('-inf'), 'ul': float('-inf')}  # peak block RMS since last stats line

    def px_open(now):
        nonlocal px_dlb, px_ulb, px_out
        px.update(on=True, start=dl_total, sent=0, cons=0, last=now, skip_dl=max(0, ul_total - dl_total),
                  skip_ul=max(0, dl_total - ul_total))
        px['opens'] += 1
        px_dlb = px_ulb = px_out = np.zeros(0, np.complex64)
        if px['opens'] > 1:
            req_out['px_tx'] = False  # REQ_RELAXED: a fresh request supersedes one lost with a dead peer
        print(f'[broker] passive lane OPEN #{px["opens"]} at DL index {dl_total}'
              + (' (DISCONTINUITY: re-open)' if px['opens'] > 1 else ''), flush=True)

    def px_close(why):
        nonlocal px_dlb, px_ulb, px_out
        px['on'] = False
        px_dlb = px_ulb = px_out = np.zeros(0, np.complex64)
        print(f'[broker] passive lane CLOSED ({why}); gNB/UE loop continues unthrottled', flush=True)

    def px_feed(dl=None, ul=None):
        nonlocal px_dlb, px_ulb, px_out
        if not px['on']:
            return
        if dl is not None:
            k = min(px['skip_dl'], len(dl)); px['skip_dl'] -= k
            px_dlb = np.concatenate((px_dlb, dl[k:]))
        if ul is not None:
            k = min(px['skip_ul'], len(ul)); px['skip_ul'] -= k
            px_ulb = np.concatenate((px_ulb, ul[k:]))
        n = min(len(px_dlb), len(px_ulb))
        if n:
            if a.no_ul_norm:
                g_ul = np.float32(g_ul_rel)
            else:
                g_ul = np.float32(g_ul_rel * np.sqrt(pmax['dl'] / pmax['ul'])) if pmax['ul'] > 0 else np.float32(0)
            mix = px_dlb[:n] * g_px + px_ulb[:n] * g_ul
            px_out = np.concatenate((px_out, mix.astype(np.complex64)))
            px_dlb, px_ulb = px_dlb[n:], px_ulb[n:]

    def on_ul(b):
        nonlocal ul_total
        ul_q.put(b)
        ul_total += len(b) // CF
        arr = np.frombuffer(b, np.complex64)
        pk['ul'] = max(pk['ul'], rms_db(arr))
        pmax['ul'] = max(pmax['ul'], float(np.mean(np.abs(arr) ** 2)) if len(arr) else 0.0)
        px_feed(ul=arr)

    poller = zmq.Poller()
    t0 = tstat = time.time()
    dl_prev = 0
    print(f'[broker] up: srate={a.srate:.0f} no_ue={a.no_ue} px_window={win} px_hwm={hwm}', flush=True)
    while True:
        now = time.time()
        # ---- issue requests ----
        px_lag = dl_total - (px['start'] + px['cons']) if px['on'] else 0
        if not req_out['gnb_dl'] and ue_q.n < hwm_ue and (not px['on'] or px_lag < hwm):
            gnb_dl.send(b'\x00'); req_out['gnb_dl'] = True
        if ue_ul is not None and ue_on and not req_out['ue_ul'] and ul_q.n < hwm_ue:
            ue_ul.send(b'\x00'); req_out['ue_ul'] = True
        if not req_out['px_tx']:
            px_tx.send(b'\x00'); req_out['px_tx'] = True
        # ---- serve owed replies ----
        if owed['ue_dl'] and ue_q.n:
            ue_dl.send(ue_q.take(SRSUE_MAX_MSG)); owed['ue_dl'] = False
        if owed['gnb_ul']:
            if not ue_on and ul_q.n == 0 and dl_total > ul_total:
                on_ul(bytes((dl_total - ul_total) * CF))
            if ul_q.n:
                gnb_ul.send(ul_q.take(OCUDU_MAX_MSG)); owed['gnb_ul'] = False
        if owed['px_dl'] and px['on']:
            n = min(len(px_out), px['cons'] + win - px['sent'], OAI_MAX_MSG)
            if n > 0:
                px_dl.send(px_out[:n].tobytes()); px_out = px_out[n:]
                px['sent'] += n; owed['px_dl'] = False
        # ---- stall detection ----
        if px['on'] and len(px_out) and now - px['last'] > a.px_stall:
            px_close(f'no progress for {a.px_stall:.0f}s')
        # ---- poll ----
        poller = zmq.Poller()
        watch = [(gnb_dl, req_out['gnb_dl']), (px_tx, req_out['px_tx']),
                 (gnb_ul, not owed['gnb_ul']), (ue_dl, not owed['ue_dl']), (px_dl, not owed['px_dl'])]
        if ue_ul is not None:
            watch.append((ue_ul, req_out['ue_ul']))
        for s, w in watch:
            if w:
                poller.register(s, zmq.POLLIN)
        ev = dict(poller.poll(5))
        if gnb_dl in ev:
            b = gnb_dl.recv(); req_out['gnb_dl'] = False
            if len(b) >= CF:
                if ue_on:
                    ue_q.put(b)
                dl_total += len(b) // CF
                arr = np.frombuffer(b, np.complex64)
                pk['dl'] = max(pk['dl'], rms_db(arr))
                pmax['dl'] = max(pmax['dl'], float(np.mean(np.abs(arr) ** 2)))
                px_feed(dl=arr)
        if ue_ul is not None and ue_ul in ev:
            b = ue_ul.recv(); req_out['ue_ul'] = False
            if len(b) >= CF:
                on_ul(b)
        if px_tx in ev:
            b = px_tx.recv(); req_out['px_tx'] = False
            if px['on']:
                px['cons'] += len(b) // CF; px['last'] = now
        if gnb_ul in ev:
            gnb_ul.recv(); owed['gnb_ul'] = True
        if ue_dl in ev:
            ue_dl.recv(); owed['ue_dl'] = True
            if not ue_on and not a.no_ue:
                ue_on = True
                ue_q.clear()
                if dl_total > ul_total:
                    on_ul(bytes((dl_total - ul_total) * CF))
                print(f'[broker] srsUE connected: its stream starts at DL index {dl_total} (UL padded to it)', flush=True)
        if px_dl in ev:
            px_dl.recv(); owed['px_dl'] = True
            if not px['on']:
                px_open(now)
            px['last'] = now
        # ---- stats ----
        if now - tstat >= a.stats:
            rt = (dl_total - dl_prev) / a.srate / (now - tstat)
            print(f'[broker] t={now - t0:7.1f}s dl={dl_total} ul={ul_total} rt={rt:.3f}x '
                  f'ueq={ue_q.n} ulq={ul_q.n} px={"on" if px["on"] else "off"} sent={px["sent"]} '
                  f'cons={px["cons"]} out={len(px_out)} lag={px_lag} '
                  f'peak_blk_rms dl={pk["dl"]:.1f} ul={pk["ul"]:.1f} dBFS', flush=True)
            tstat, dl_prev = now, dl_total
            pk['dl'] = pk['ul'] = float('-inf')


if __name__ == '__main__':
    main()
