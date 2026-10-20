#!/usr/bin/env python3
"""SYSAN: per-thread and per-core CPU budget of a running receiver (no perf needed).
usage: sysan_threads.py EXE_PREFIX OUT.json [SECONDS=60] [START_DELAY=0] [LOG REGEX]
Waits for an nr-uesoftmodem whose exe starts with EXE_PREFIX, sleeps START_DELAY, then samples
/proc/<pid>/task/*/stat and /proc/stat over SECONDS. Writes per thread: comm, last core, policy, rt prio,
nice, CPU % of one core over the window, allowed CPUs, wakeups/s (voluntary context switches), involuntary
switches/s, run-queue wait % (schedstat); per core: busy % and the threads that ran there. Also writes OUT.ps.txt
(ps -L tid,psr,pcpu,policy,rtprio,comm at the start and end of the window). Reads /proc only; no perf, no ptrace.
On air (main, during an OTA run):  python3 sysan_threads.py /home/nicola/ OUT.json 90 40"""
import json, os, subprocess, sys, time


def find_pid(prefix):
    # the receiver can show as two processes (a short-lived helper with one thread): take the one with the most threads
    best = None
    for p in subprocess.run(['pgrep', '-x', 'nr-uesoftmodem'], capture_output=True, text=True).stdout.split():
        try:
            exe = subprocess.run(['sudo', '-n', 'readlink', '/proc/%s/exe' % p], capture_output=True, text=True).stdout.strip()
        except OSError:
            continue
        if exe.startswith(prefix):
            try:
                n = len(os.listdir('/proc/%s/task' % p))
            except OSError:
                continue
            if best is None or n > best[0]:
                best = (n, int(p))
    return best[1] if best and best[0] > 4 else None


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
        d = dict(comm=comm, ticks=int(f[11]) + int(f[12]), nice=int(f[16]), cpu=int(f[36]), rtprio=int(f[37]), policy=int(f[38]),
                 vcs=0, ncs=0, wait_ns=0, allowed='?')
        try:
            for l in open('/proc/%d/task/%s/status' % (pid, t)):
                k, _, v = l.partition(':')
                if k == 'voluntary_ctxt_switches': d['vcs'] = int(v)
                elif k == 'nonvoluntary_ctxt_switches': d['ncs'] = int(v)
                elif k == 'Cpus_allowed_list': d['allowed'] = v.strip()
            d['wait_ns'] = int(open('/proc/%d/task/%s/schedstat' % (pid, t)).read().split()[1])
        except (OSError, IndexError, ValueError):
            pass
        out[int(t)] = d
    return out


def ps_snap(pid, tag, path):
    r = subprocess.run(['ps', '-L', '-o', 'tid,psr,pcpu,policy,rtprio,comm', '-p', str(pid)], capture_output=True, text=True)
    with open(path, 'a') as f:
        f.write('## %s %s\n%s' % (tag, time.strftime('%H:%M:%S'), r.stdout))


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
    if len(sys.argv) > 6:  # LOG REGEX: start the window when the regex first appears in LOG (e.g. sync), then START_DELAY
        import re
        rx, t_log = re.compile(sys.argv[6].encode()), time.time() + 900
        while time.time() < t_log:
            try:
                if rx.search(open(sys.argv[5], 'rb').read()):
                    break
            except OSError:
                pass
            time.sleep(0.5)
    time.sleep(delay)
    pid = find_pid(prefix) or pid  # re-pick after the delay: start-up helpers and restarts change the pid
    hz = os.sysconf('SC_CLK_TCK')
    ps_snap(pid, 'start', out + '.ps.txt')
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
    ps_snap(pid, 'end', out + '.ps.txt')
    pol = {0: 'OTHER', 1: 'FIFO', 2: 'RR', 3: 'BATCH', 5: 'IDLE'}
    th = []
    zero = dict(ticks=0, vcs=0, ncs=0, wait_ns=0)
    for tid, v in b.items():
        a0 = a.get(tid, zero)
        d = (v['ticks'] - a0['ticks']) / hz
        sc = seen_cpus.get(tid, {})
        n = max(1, sum(sc.values()))
        th.append(dict(tid=tid, comm=v['comm'], cores=sorted(sc, key=lambda k: -sc[k])[:3],
                       core_share={k: round(sc[k] / n, 2) for k in sc}, allowed=v['allowed'],
                       policy=pol.get(v['policy'], v['policy']), rtprio=v['rtprio'], nice=v['nice'], pct=round(100 * d / wall, 1),
                       wake_s=round((v['vcs'] - a0['vcs']) / wall), invol_s=round((v['ncs'] - a0['ncs']) / wall),
                       rqwait_pct=round(100 * (v['wait_ns'] - a0['wait_ns']) / 1e9 / wall, 1)))
    th.sort(key=lambda x: -x['pct'])
    core = {k: round(100.0 * (cb[k][0] - ca[k][0]) / max(1, cb[k][1] - ca[k][1]), 1) for k in cb}
    # per core: receiver CPU % attributed by each thread's sampled core shares (approximate: psr is sampled every 0.25 s)
    per_core = {}
    for x in th:
        for k, sh in x['core_share'].items():
            if x['pct'] * sh >= 0.5:
                per_core.setdefault(k, []).append('%s:%.0f' % (x['comm'], x['pct'] * sh))
    json.dump(dict(wall_s=round(wall, 1), threads=th, cores=core, per_core=per_core), open(out, 'w'), indent=1)
    for x in th:
        if x['pct'] >= 0.5 or x['wake_s'] >= 100:
            print('%-16s tid=%-8d cores=%-10s allowed=%-8s %-5s prio=%-3d nice=%-3d %5.1f %% wake/s=%-6d invol/s=%-5d rqwait=%.1f %%' % (
                x['comm'], x['tid'], ','.join(map(str, x['cores'])), x['allowed'], x['policy'], x['rtprio'], x['nice'], x['pct'],
                x['wake_s'], x['invol_s'], x['rqwait_pct']))
    print('cores busy %:', ' '.join('%d:%.0f' % (k, core[k]) for k in sorted(core)))
    for k in sorted(per_core):
        print('core %2d: %s' % (k, ' '.join(per_core[k])))


if __name__ == '__main__':
    main()
