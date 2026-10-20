#!/usr/bin/env python3
"""Fake receiver starts the dashboard: non-root, pinned, niced + idle class, single instance per port, replaced on a new run."""
import os
import socket
import subprocess
import sys
import tempfile
import time
import urllib.request

binary, monitor = sys.argv[1:]
core = max(os.sched_getaffinity(0))
with socket.socket() as s:
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
env = dict(os.environ)
if os.geteuid() == 0:  # run as root: the spawn must still drop to SUDO_UID
    env["SUDO_UID"] = env["SUDO_GID"] = "65534"
tmp = tempfile.mkdtemp(prefix="rxdash_spawn_")
runs = []
for n in ("runA", "runB"):
    d = os.path.join(tmp, n)
    os.mkdir(d)
    open(os.path.join(d, "run.log"), "w").write("[PHY]   Initial sync successful, PCI: 1\n")
    os.chmod(d, 0o777)
    runs.append(d)


def start(run):
    r = subprocess.run([binary, run, str(port), str(core), monitor], env=env, capture_output=True, text=True, timeout=10)
    assert r.returncode == 0, r.stdout + r.stderr
    return r.stdout


def holder():
    for _ in range(100):
        try:
            pid, key = open("/tmp/rxdash-%d-%d.lock" % (65534 if os.geteuid() == 0 else os.getuid(), port)).read().split(" ", 1)
            if os.path.exists("/proc/%s" % pid):
                return int(pid), key
        except (OSError, ValueError):
            pass
        time.sleep(0.1)
    raise AssertionError("dashboard did not take its lock")


def instances():
    out = []
    for p in os.listdir("/proc"):
        try:
            c = open("/proc/%s/cmdline" % p, "rb").read().split(b"\0")
            if b"--single" in c and str(port).encode() in c and c[1] == monitor.encode():
                out.append(int(p))
        except OSError:
            pass
    return out


assert "http://127.0.0.1:%d" % port in start(runs[0])
pid, key = holder()
st = {l.split(":")[0]: l.split(":", 1)[1].split() for l in open("/proc/%d/status" % pid) if ":" in l}
expect_uid = "65534" if os.geteuid() == 0 else str(os.getuid())
assert st["Uid"][:3] == [expect_uid] * 3, st["Uid"]
assert st["Cpus_allowed_list"] == [str(core)], st["Cpus_allowed_list"]
stat = open("/proc/%d/stat" % pid).read().rsplit(")", 1)[1].split()
assert int(stat[16]) == 19, "nice %s" % stat[16]  # field 19 (nice)
assert os.sched_getscheduler(pid) == os.SCHED_IDLE
assert os.getsid(pid) == pid, "not detached"
for _ in range(50):  # serving
    try:
        assert urllib.request.urlopen("http://127.0.0.1:%d/" % port, timeout=2).status == 200
        break
    except OSError:
        time.sleep(0.2)
else:
    raise AssertionError("not serving")
start(runs[0])  # same run: reused, never a second instance
time.sleep(1.5)
assert instances() == [pid], (instances(), pid)
start(runs[1])  # next receiver start: replaces
for _ in range(100):
    if not os.path.exists("/proc/%d" % pid):
        break
    time.sleep(0.1)
pid2, key2 = holder()
assert pid2 != pid and key2.endswith("runB") and instances() == [pid2], (pid2, key2, instances())
os.kill(pid2, 15)
print("nr_dashboard OK")
