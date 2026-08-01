#!/usr/bin/env python3
"""Low-CPU downlink UDP traffic generator, drop-in replacement for `iperf3 -u -b <rate>`.

WHY THIS EXISTS: iperf3 burns a FULL CORE per stream regardless of rate or --pacing-timer
(measured 100-102% CPU for a 6 Mbit/s UDP stream at pacing-timer 1000/20000/50000 us, and in TCP
mode too). With three active UEs that is three of twelve cores spent generating 18 Mbit/s -- on a
box where the rfsimulator federation is the bottleneck and was only reaching ~3% of real time.
This sender paces with an absolute-deadline sleep loop instead of spinning, costing ~1-2% CPU for
the same offered load, and the receiver blocks in recvfrom() so it costs nothing when idle.

The traffic itself is deliberately boring -- constant-rate UDP with a sequence number -- because
all the sensing pipeline needs from it is that the gNB keeps issuing DL grants (which is what the
blind-PDCCH source decodes and what keeps PDSCH DM-RS on the air). Throughput fidelity and jitter
statistics are not being measured, so none of iperf3's machinery is needed.

Usage:
  udp_dl.py send --dst <ip> [--port 5201] [--rate 6M] [--dur 60] [--pkt 1200]
  udp_dl.py recv [--bind 0.0.0.0] [--port 5201] [--dur 60]

Prints a one-line summary on exit so a harness can confirm traffic actually flowed.
"""
import argparse, socket, sys, time


def parse_rate(s):
    s = str(s).strip().upper()
    mult = 1
    if s.endswith("K"):
        mult, s = 1_000, s[:-1]
    elif s.endswith("M"):
        mult, s = 1_000_000, s[:-1]
    elif s.endswith("G"):
        mult, s = 1_000_000_000, s[:-1]
    return float(s) * mult


def send(a):
    bits_per_pkt = a.pkt * 8
    pps = max(1.0, parse_rate(a.rate) / bits_per_pkt)
    # Send in small bursts so the wakeup rate stays low even at high pps: one burst every
    # ~BURST_MS regardless of rate, which is what keeps CPU flat instead of scaling with pps.
    BURST_MS = 20.0
    per_burst = max(1, int(round(pps * BURST_MS / 1000.0)))
    interval = per_burst / pps

    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF, 1 << 20)
    payload = bytearray(a.pkt)
    dst = (a.dst, a.port)

    t0 = time.monotonic()
    deadline = t0
    end = t0 + a.dur
    sent = 0
    errs = 0
    while True:
        now = time.monotonic()
        if now >= end:
            break
        # Absolute-deadline pacing: never accumulates drift, and sleeps rather than spins.
        deadline += interval
        for _ in range(per_burst):
            payload[0:4] = (sent & 0xFFFFFFFF).to_bytes(4, "big")
            try:
                s.sendto(payload, dst)
                sent += 1
            except OSError:
                errs += 1
        slack = deadline - time.monotonic()
        if slack > 0:
            time.sleep(slack)
        else:
            # Fell behind (host or link congested): resync rather than spiral.
            deadline = time.monotonic()
    dt = time.monotonic() - t0
    print(f"udp_dl send: {sent} pkts, {errs} errors, {sent*bits_per_pkt/dt/1e6:.2f} Mbit/s over {dt:.1f}s "
          f"to {a.dst}:{a.port} (burst={per_burst}/{interval*1000:.1f}ms)")


def recv(a):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 1 << 22)
    # Retry the bind: the receiver is started right after the UE reports its IP, but the address is
    # not necessarily configured on oaitun_ue1 yet, and bind() then fails with EADDRNOTAVAIL. That
    # happened silently on every run until 2026-07-29 (udp_server_*.log held a bare traceback that
    # nothing checked), so the UDP receivers were simply never up. Retry, then fall back to
    # INADDR_ANY rather than dying -- a receiver on 0.0.0.0 still absorbs the traffic, which is all
    # this needs to do.
    deadline = time.monotonic() + 60.0
    while True:
        try:
            s.bind((a.bind, a.port))
            break
        except OSError as e:
            if time.monotonic() >= deadline:
                print(f"udp_dl recv: could not bind {a.bind}:{a.port} ({e}); falling back to 0.0.0.0",
                      flush=True)
                s.bind(("0.0.0.0", a.port))
                break
            time.sleep(0.5)
    s.settimeout(1.0)
    end = time.monotonic() + a.dur
    n = 0
    nbytes = 0
    while time.monotonic() < end:
        try:
            d, _ = s.recvfrom(65535)   # blocking: ~0 CPU while waiting
            n += 1
            nbytes += len(d)
        except socket.timeout:
            continue
        except OSError:
            break
    print(f"udp_dl recv: {n} pkts, {nbytes/1e6:.2f} MB on {a.bind}:{a.port}")


p = argparse.ArgumentParser()
sub = p.add_subparsers(dest="mode", required=True)
ps = sub.add_parser("send")
ps.add_argument("--dst", required=True)
ps.add_argument("--port", type=int, default=5201)
ps.add_argument("--rate", default="6M")
ps.add_argument("--dur", type=float, default=60)
ps.add_argument("--pkt", type=int, default=1200)
ps.set_defaults(fn=send)
pr = sub.add_parser("recv")
pr.add_argument("--bind", default="0.0.0.0")
pr.add_argument("--port", type=int, default=5201)
pr.add_argument("--dur", type=float, default=60)
pr.set_defaults(fn=recv)
a = p.parse_args()
try:
    a.fn(a)
except KeyboardInterrupt:
    sys.exit(0)
