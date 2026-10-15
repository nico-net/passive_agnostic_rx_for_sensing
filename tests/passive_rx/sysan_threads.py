#!/usr/bin/env python3
"""SYSAN: per-thread and per-core CPU budget of a running receiver (no perf needed).
usage: sysan_threads.py EXE_PREFIX OUT.json [SECONDS=60] [START_DELAY=0]
Waits for an nr-uesoftmodem whose exe starts with EXE_PREFIX, sleeps START_DELAY, then samples
/proc/<pid>/task/*/stat and /proc/stat over SECONDS. Writes per thread: comm, last core, policy, rt prio,
nice, CPU % of one core over the window; per core: busy %."""
import json, os, subprocess, sys, time


def find_pid(prefix):
    for p in subprocess.run(['pgrep', '-x', 'nr-uesoftmodem'], capture_output=True, text=True).stdout.split():
        try:
            exe = subprocess.run(['sudo', '-n', 'readlink', '/proc/%s/exe' % p], capture_output=True, text=True).stdout.strip()
        except OSError:
            continue
        if exe.startswith(prefix):
            return int(p)
    return None


def tasks(pid):
    out = {}
    for t in os.listdir('/proc/%d/task' % pid):
        try:
            s = open('/proc/%d/task/%s/stat' % (pid, t)).read()
        except OSError:
            continue
        comm = s[s.index('(') + 1:s.rindex(')')]
        f = s[s.rindex(')') + 2:].split()
        # fields after comm: state(0) ... utime(11) stime(12) ... nice(16) ... processor(36) rt_priority(37) policy(38)
        out[int(t)] = dict(comm=comm, ticks=int(f[11]) + int(f[12]), nice=int(f[16]), cpu=int(f[36]), rtprio=int(f[37]), policy=int(f[38]))
    return out


def cores():
    c = {}
    for l in open('/proc/stat'):
        if l.startswith('cpu') and l[3].isdigit():
            v = list(map(int, l.split()[1:]))
            c[int(l.split()[0][3:])] = (sum(v) - v[3] - v[4], sum(v))
    return c


def main():
    prefix, out = sys.argv[1], sys.argv[2]
    secs = float(sys.argv[3]) if len(sys.argv) > 3 else 60
    delay = float(sys.argv[4]) if len(sys.argv) > 4 else 0
    t_end = time.time() + 3600
    pid = None
    while pid is None and time.time() < t_end:
        pid = find_pid(prefix)
        time.sleep(0.2)
    if pid is None:
        sys.exit('no receiver')
    time.sleep(delay)
    hz = os.sysconf('SC_CLK_TCK')
    a, ca, w0 = tasks(pid), cores(), time.time()
    seen_cpus = {tid: {} for tid in a}
    b, cb = {}, ca
    while time.time() - w0 < secs:
        time.sleep(0.25)
        try:
            cur = tasks(pid)
        except FileNotFoundError:
            break
        if len(cur) < 0.8 * max(1, len(b)):  # shutting down: keep the last full snapshot
            break
        b.update(cur)
        cb, wall = cores(), time.time() - w0
        for tid, v in cur.items():
            seen_cpus.setdefault(tid, {})
            seen_cpus[tid][v['cpu']] = seen_cpus[tid].get(v['cpu'], 0) + 1
    pol = {0: 'OTHER', 1: 'FIFO', 2: 'RR', 3: 'BATCH', 5: 'IDLE'}
    th = []
    for tid, v in b.items():
        d = (v['ticks'] - a.get(tid, dict(ticks=0))['ticks']) / hz
        th.append(dict(tid=tid, comm=v['comm'], cores=sorted(seen_cpus.get(tid, {}), key=lambda k: -seen_cpus[tid][k])[:3],
                       policy=pol.get(v['policy'], v['policy']), rtprio=v['rtprio'], nice=v['nice'], pct=round(100 * d / wall, 1)))
    th.sort(key=lambda x: -x['pct'])
    core = {k: round(100.0 * (cb[k][0] - ca[k][0]) / max(1, cb[k][1] - ca[k][1]), 1) for k in cb}
    json.dump(dict(wall_s=round(wall, 1), threads=th, cores=core), open(out, 'w'), indent=1)
    for x in th:
        if x['pct'] >= 0.5:
            print('%-16s tid=%-8d cores=%-10s %-5s prio=%-3d nice=%-3d %5.1f %%' % (x['comm'], x['tid'], ','.join(map(str, x['cores'])),
                                                                              x['policy'], x['rtprio'], x['nice'], x['pct']))
    print('cores busy %:', ' '.join('%d:%.0f' % (k, core[k]) for k in sorted(core)))


if __name__ == '__main__':
    main()
