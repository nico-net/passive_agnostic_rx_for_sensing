#!/usr/bin/env python3
"""Passive FR1 broadcast acquisition. Hardware/search bounds are not cell facts."""
import argparse
import bisect
import fcntl
import json
import math
import os
from pathlib import Path
import re
import signal
import socket
import subprocess
import sys
import time

REPO = Path(__file__).resolve().parents[2]
BW_MHZ = (5, 10, 15, 20, 25, 30, 35, 40, 45, 50, 60, 70, 80, 90, 100)
FAULT = re.compile(r"RFSTALL|RFTSDISC|RXDISCONT|rx xport timed out|No CHDR|"
                   r"Assertion|can't open radio device|device_init failed|RF AFFINITY FAILED")
ANSI = re.compile(r"\x1b\[[0-9;]*[A-Za-z]")


class VoidRun(RuntimeError):
    pass


class Unsupported(RuntimeError):
    pass


def save(path, value):
    temporary = path.with_suffix(path.suffix + '.tmp')
    temporary.write_text(json.dumps(value, indent=2) + '\n')
    temporary.replace(path)


def standard_tables():
    # Use the repository's standards tables, never a gNB config or capture log.
    source = (REPO / 'common/utils/nr/nr_common.c').read_text()
    raster = re.search(r'const sync_raster_t sync_raster\[\]\s*=\s*\{(.*?)\n\};', source, re.S)
    bandwidth = re.search(r'static const int tables_5_3_2\[5\]\[NUM_BW_ENTRIES\]\s*=\s*\{(.*?)\n\};', source, re.S)
    if not raster or not bandwidth:
        raise RuntimeError('Standard table format changed; refusing to guess a scan plan')
    rows = [tuple(map(int, row)) for row in re.findall(
        r'\{\s*(\d+),\s*(\d+),\s*(\d+),\s*(\d+),\s*(\d+)\s*\}', raster[1])]
    bw = [tuple(map(int, re.findall(r'-?\d+', row)))
          for row in re.findall(r'\{([^{}]+)\}', bandwidth[1])]
    if len(bw) != 5 or any(len(row) != len(BW_MHZ) for row in bw):
        raise RuntimeError('Unsupported bandwidth table format')
    return rows, bw


def ss_ref(gscn):
    if 2 <= gscn <= 7498:
        for m in (1, 3, 5):
            numerator = gscn - (m - 3) // 2
            if numerator % 3 == 0:
                return (numerator // 3) * 1200000 + m * 50000
    elif 7499 <= gscn <= 22255:
        return 3000000000 + (gscn - 7499) * 1440000
    raise Unsupported('Outside the supported FR1 synchronization raster')


def arfcn_hz(arfcn):
    if 0 <= arfcn <= 599999:
        return arfcn * 5000
    if 600000 <= arfcn <= 2016666:
        return 3000000000 + (arfcn - 600000) * 15000
    raise Unsupported('UL Point A is outside FR1')


def scan_plan(args, rows, bandwidths):
    plans = []
    for band, mu, first, step, last in rows:
        if band >= 254 or mu not in (0, 1):
            continue  # No FR2 or NTN claim.
        points = [ss_ref(g) for g in range(first, last + 1, step)
                  if args.start_hz <= ss_ref(g) <= args.stop_hz]
        if not points:
            continue
        prb = max(rb for rb, mhz in zip(bandwidths[mu], BW_MHZ)
                  if rb > 0 and mhz <= args.max_bandwidth_mhz)
        scs = 15000 << mu
        reach = prb * 6 * scs - 120 * scs - args.cfo_max_hz
        if reach <= 0:
            raise Unsupported('RF bandwidth too small for SSB and CFO search margin')
        i = 0
        while i < len(points):
            j = bisect.bisect_right(points, points[i] + reach) - 1
            center = points[j]
            plans.append(dict(band=band, mu=mu, prb=prb, center_hz=center))
            i = bisect.bisect_right(points, center + reach)
    return sorted(plans, key=lambda p: (p['center_hz'], p['mu'], p['band']))


def geometry(ssb, sib, args, bandwidths, rows):
    if ssb['pci'] != sib['pci']:
        raise Unsupported('SSB/SIB1 timing identity mismatch')
    mu = sib['dl_mu']
    if mu not in (0, 1) or ssb['ssb_mu'] != mu or sib['mib_mu'] != mu or sib['ul_mu'] != mu:
        raise Unsupported('Mixed SSB/DL/UL numerologies are not supported by this receiver')
    if not sib['tdd']:
        raise Unsupported('Simultaneous passive DL/UL currently requires a co-channel TDD carrier')
    if sib['dl_offset_rb'] != 0 or sib['ul_offset_rb'] != 0:
        raise Unsupported('Nonzero offsetToCarrier needs a PHY grid-contract extension')
    if sib['dl_prb'] != sib['ul_prb']:
        raise Unsupported('Different DL/UL grid widths need a separate capture path')
    if not (0 <= sib['k_ssb'] <= 23 and 0 <= sib['offset_to_point_a_rb'] <= 2199):
        raise Unsupported('Invalid or non-associated SSB/Point A')
    scs = 15000 << mu
    # TS 38.211 4.4.4.2 and 7.4.3.1: FR1 offsetToPointA and kSSB
    # are in 15 kHz reference units, NOT in the trial capture numerology.
    lower_ssb = ssb['ss_ref_hz'] - 120 * scs
    ssb_offset_hz = (12 * sib['offset_to_point_a_rb'] + sib['k_ssb']) * 15000
    point_a = lower_ssb - ssb_offset_hz
    if point_a <= 0 or ssb_offset_hz % scs:
        raise Unsupported('SSB is not integer-aligned to the supported FFT grid')
    if sib['ul_point_a_arfcn'] >= 0 and arfcn_hz(sib['ul_point_a_arfcn']) != point_a:
        raise Unsupported('UL and DL do not share Point A')
    prb = sib['dl_prb']
    if prb not in bandwidths[mu]:
        raise Unsupported('Broadcast bandwidth is not supported by the local PHY')
    mhz = BW_MHZ[bandwidths[mu].index(prb)]
    if mhz > args.max_bandwidth_mhz:
        raise Unsupported('Broadcast bandwidth exceeds configured RF capability')
    first_sc = ssb_offset_hz // scs
    if first_sc < 0 or first_sc + 240 > prb * 12:
        raise Unsupported('SSB is outside the discovered carrier grid')
    allowed = any(b == sib['band'] and m == mu and any(
        ss_ref(g) == ssb['ss_ref_hz'] for g in range(first, last + 1, step))
        for b, m, first, step, last in rows if b < 254 and m in (0, 1))
    if not allowed:
        raise Unsupported('Decoded band and measured SSB raster disagree')
    return dict(band=sib['band'], mu=mu, prb=prb,
                center_hz=point_a + prb * 6 * scs, point_a_hz=point_a,
                bandwidth_mhz=mhz, ssb_sc=first_sc, pci=ssb['pci'],
                ss_ref_hz=ssb['ss_ref_hz'])


def receiver_config(out, probe):
    if probe:
        return 'sensing = { enable = 0; };\n'
    # These are decoder compute/search limits and host affinities, not cell hints.
    return '''sensing = {
  enable = 1;
  pdcch_blind_monitor_autoconf = 1;
  pdcch_blind_monitor_autodiscover = 1;
  pdcch_blind_monitor_full_auto = 1;
  pdcch_blind_monitor_dci10 = "1";
  pdcch_blind_monitor_pdsch = "2:1:0:1:16:2:64:6";
  pdcch_blind_monitor_rnti_range = "1:65519";
  pdcch_blind_monitor_noise_gates = "0:2:500:0:3.0";
  pdcch_blind_monitor_scan_thread = "1:8:5";
  pdcch_blind_monitor_dci01 = "1:0";
  pdcch_blind_monitor_ul_pusch = "1:1:0";
  pdcch_blind_monitor_ul_uci = "256:11:0:32";
  pdcch_blind_monitor_ul_thread = "2:64:2";
  sources = "csi_rs,ssb,pdsch_dmrs_blind,pdsch_data,pusch_dmrs,pusch_data";
  cpi_slots = 32;
  interpolate = 1;
  capture = 1;
  sync_correction = 1;
  nominal_los_range_m = 0.0;
  report_path = %s;
  out_path = %s;
  report_endpoint = "tcp://127.0.0.1:5556";
  rx_id = "rx_x410";
};
''' % (json.dumps(str(out / 'reports.jsonl')), json.dumps(str(out / 'sensing')))


class Acquisition:
    def __init__(self, args, rows, bandwidths):
        self.args, self.rows, self.bandwidths = args, rows, bandwidths
        self.out = args.out.resolve()
        self.out.mkdir(parents=True, exist_ok=False)
        self.runlog = (self.out / 'run.log').open('a', buffering=1)
        self.sequence = 0
        self.child = None
        self.monitor = None
        self.lock_fd = None

    def state(self, state, **values):
        record = dict(state=state, wall_time=time.time(), **values)
        save(self.out / 'acquisition.json', record)
        print(json.dumps(record), flush=True)

    def nic_missed(self):
        path = Path('/sys/class/net') / self.args.interface / 'statistics/rx_missed_errors'
        try:
            return int(path.read_text())
        except (OSError, ValueError) as error:
            raise VoidRun(f'Cannot account for RF NIC drops: {error}') from error

    @staticmethod
    def stop(process):
        if process is None or process.poll() is not None:
            return
        os.killpg(process.pid, signal.SIGINT)
        try:
            process.wait(timeout=3)
        except subprocess.TimeoutExpired:
            # OAI's first SIGINT requests NAS deregistration, even for a passive
            # receiver with no GUTI. Its second SIGINT requests actual shutdown.
            try:
                os.killpg(process.pid, signal.SIGINT)
            except ProcessLookupError:
                process.wait()
                return
            try:
                process.wait(timeout=9)
            except subprocess.TimeoutExpired:
                os.killpg(process.pid, signal.SIGKILL)
                process.wait()
                raise VoidRun('Receiver required SIGKILL; no automatic radio reuse or reboot')

    def acquire_radio_lock(self):
        lock_path = '/tmp/adaptive-rx-UL-DL.radio.lock'
        # protected_regular forbids root O_CREAT on another user's existing
        # file in /tmp. Open existing locks without O_CREAT, preserving the
        # inode shared with the older launchers. Never unlink a lock file.
        flags = os.O_RDWR | os.O_NOFOLLOW | os.O_CLOEXEC
        try:
            self.lock_fd = os.open(lock_path, flags)
        except FileNotFoundError:
            try:
                self.lock_fd = os.open(lock_path, flags | os.O_CREAT | os.O_EXCL, 0o600)
            except FileExistsError:
                self.lock_fd = os.open(lock_path, flags)
        fcntl.flock(self.lock_fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
        for entry in Path('/proc').iterdir():
            if not entry.name.isdigit():
                continue
            try:
                executable = (entry / 'exe').resolve(strict=True).name
            except (OSError, RuntimeError):
                continue
            if executable in ('nr-uesoftmodem', 'nr-softmodem'):
                raise VoidRun(f'Existing radio owner PID {entry.name}; stop it explicitly first')

    def start_monitor(self):
        # Loopback by default; a LAN bind is an explicit operator choice.
        # Never replace another user's monitor.
        with socket.socket() as sock:
            sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            sock.bind((self.args.monitor_bind, self.args.monitor_port))
        with (self.out / 'dashboard.log').open('w') as log:
            self.monitor = subprocess.Popen(
                [sys.executable, str(REPO / 'tests/passive_rx/monitor/monitor.py'),
                 '--connect', 'tcp://127.0.0.1:5556', '--log', str(self.out / 'run.log'),
                 '--port', str(self.args.monitor_port), '--bind', self.args.monitor_bind],
                stdout=log, stderr=subprocess.STDOUT, start_new_session=True)
        (self.out / "dashboard.pid").write_text(str(self.monitor.pid) + "\n")
        self.state('SCANNING', dashboard=f'http://{self.args.monitor_bind}:{self.args.monitor_port}/')

    def attempt(self, candidate, probe, expected=None, seed=None):
        self.sequence += 1
        directory = self.out / f'{self.sequence:04d}-{"probe" if probe else "capture"}'
        directory.mkdir()
        config = directory / 'receiver.conf'
        config.write_text(receiver_config(self.out, probe))
        env = dict(PATH='/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin',
                   HOME=str(Path.home()), LANG='C', LD_LIBRARY_PATH=f'{self.args.binary.parent}:/usr/local/lib',
                   ISAC_AUTO_ACQUIRE='1', ISAC_ACQ_PROBE=str(int(probe)),
                   ISAC_ACQ_CFO_MAX_HZ=str(self.args.cfo_max_hz),
                   ISAC_SYNC_ONLY='1', ISAC_RX_MRC_MODE='2', ISAC_UL_RX_BRANCH='-1',
                   ISAC_DMRS_FO_APPLY='0', ISAC_SFO_CORRECT='0', ISAC_RX_BRANCH_FO='0',
                   ISAC_RX_GAIN_TRIM='0,0,0,0', ISAC_DISC_NO_RESYNC='0',
                   ISAC_RF_STALL_MAX_REINIT='0', ISAC_CFO_TRACK_HZ='1',
                   ISAC_CFO_TRACK_PERIOD='20', ISAC_PDCCH_TIMING='1',
                   ISAC_PUSCH_TIMING='1', ISAC_PUSCH_DIAG='1',
                   ISAC_UL_TA_SWEEP='0:0:0', ISAC_SENSE_COMB='0', ISAC_TSYNC_RESET='0')
        # Only a PBCH measurement returned by this acquisition can seed handoff.
        initial_fo = 0
        if seed is not None:
            measured = seed["cfo_hz"]
            if not math.isfinite(measured) or abs(measured) > self.args.cfo_max_hz:
                raise VoidRun("Measured CFO is outside the configured search bound")
            if "pci" in candidate and seed["pci"] != candidate["pci"]:
                raise VoidRun("CFO seed belongs to a different measured cell")
            initial_fo = int(round(measured))
        command = ['taskset', '-c', '0,1,4', str(self.args.binary),
                   '--usrp-args', self.args.usrp_args, '-O', str(config),
                   '-r', str(candidate['prb']), '--numerology', str(candidate['mu']),
                   '--band', str(candidate['band']), '-C', str(candidate['center_hz']),
                   '--ue-rxgain', str(self.args.rx_gain), '--ue-nb-ant-rx', '4',
                   '--ue-nb-ant-tx', '4', '--passive-rx', '--ue-fo-compensation',
                   '--cont-fo-comp', '1', '--freq-sync-P', '.05', '--freq-sync-I', '.001',
                   '--initial-fo', str(initial_fo), '--ntn-initial-time-drift', '0',
                   '--thread-pool', '0,1,6,7', '--time-sync-I', '.01', '-A', '90']
        if 'ssb_sc' in candidate:
            # This value was measured during this acquisition, never read from a deployment config.
            command += ['--ssb', str(candidate['ssb_sc'])]
        else:
            command += ['--ue-scan-carrier']
        save(directory / 'invocation.json', dict(command=command, env=env, hypothesis=candidate,
                                                          cfo_seed_from_received_pbch=seed))
        self.state('PROBING' if probe else 'REACQUIRING', hypothesis=candidate)
        baseline = self.nic_missed()
        start = time.monotonic()
        last_ssb = sib = None
        accepted = False
        capture_started = None
        sleep_count = 0
        pinned = set()
        partial = ''
        logpath = directory / 'receiver.log'
        with logpath.open('w') as output, logpath.open() as reader:
            self.child = subprocess.Popen(command, cwd=directory, env=env, stdout=output,
                                          stderr=subprocess.STDOUT, start_new_session=True)
            try:
                while True:
                    age = time.monotonic() - start
                    if self.monitor.poll() is not None:
                        raise VoidRun('Dashboard exited; capture stopped rather than leaving an invisible run')
                    if self.nic_missed() != baseline:
                        raise VoidRun('RF NIC missed samples')
                    for entry in (Path('/proc') / str(self.child.pid) / 'task').glob('*'):
                        try:
                            name = (entry / 'comm').read_text().strip()
                            cpu = 0 if name.startswith('uhd_ctrl_') else 4 if name == 'UEthread_0' else None
                            if cpu is not None and entry.name not in pinned:
                                os.sched_setaffinity(int(entry.name), {cpu})
                                pinned.add(entry.name)
                        except (FileNotFoundError, ProcessLookupError):
                            continue
                    chunk = reader.read(262144)
                    if chunk:
                        self.runlog.write(chunk)
                        partial += ANSI.sub('', chunk)
                        lines = partial.split('\n')
                        partial = lines.pop()
                        # Osleep often has no newline; inspect new chunks too.
                        sleep_count += chunk.count('sleep...')
                        if sleep_count >= 10:
                            raise VoidRun('RF first-buffer stall (Osleep), not an acquisition/CRC result')
                        if FAULT.search(chunk):
                            raise VoidRun('RF/process fault; see receiver.log')
                        for line in lines:
                            if FAULT.search(line):
                                raise VoidRun('RF/process fault; see receiver.log')
                            if 'ISAC_ACQ_UNSUPPORTED:' in line:
                                raise Unsupported(line.split('ISAC_ACQ_UNSUPPORTED:', 1)[1].strip())
                            if 'Starting sync detection' in line or 'Starting re-sync detection' in line:
                                last_ssb = sib = None
                                if accepted:
                                    accepted = False
                                    self.state('REACQUIRING', hypothesis=candidate)
                            if 'ISAC_ACQ_SSB ' in line:
                                last_ssb = json.JSONDecoder().raw_decode(line.split('ISAC_ACQ_SSB ', 1)[1])[0]
                                sib = None
                            if 'ISAC_ACQ_SIB1 ' in line:
                                sib = json.JSONDecoder().raw_decode(line.split('ISAC_ACQ_SIB1 ', 1)[1])[0]
                        if last_ssb and sib and not accepted:
                            facts = geometry(last_ssb, sib, self.args, self.bandwidths, self.rows)
                            if expected is not None and facts != expected:
                                raise VoidRun('Broadcast geometry changed across acquisition handoff')
                            evidence = dict(ssb=last_ssb, sib1=sib, geometry=facts,
                                            source='received_PBCH_and_SIB1', wall_time=time.time())
                            save(directory / 'broadcast.json', evidence)
                            if probe:
                                return facts, last_ssb
                            save(self.out / 'acquired.json', evidence)
                            accepted = True
                            if capture_started is None:
                                capture_started = time.monotonic()
                            self.state('ACQUIRED', **evidence)
                        if probe and 'synch Failed:' in chunk and last_ssb is None:
                            self.state('NO_PBCH_IN_OBSERVATION', hypothesis=candidate)
                            return None, None
                    if self.child.poll() is not None:
                        raise VoidRun(f'Receiver exited with code {self.child.returncode}')
                    if probe and age >= self.args.dwell_seconds:
                        self.state('INCONCLUSIVE', reason='Probe deadline without SIB1; not proof of no cell',
                                   hypothesis=candidate, ssb=last_ssb)
                        return None, last_ssb
                    if not probe and not accepted and age >= self.args.dwell_seconds:
                        raise VoidRun('No broadcast-validated lock within capture acquisition deadline')
                    if (not probe and capture_started is not None and self.args.duration_seconds
                            and time.monotonic() - capture_started >= self.args.duration_seconds):
                        self.state('FINISHED', broadcast_validated=accepted)
                        return None, last_ssb
                    time.sleep(.25)
            finally:
                self.stop(self.child)
                self.child = None
                save(directory / 'transport.json', dict(nic_missed_before=baseline,
                                                         nic_missed_after=self.nic_missed()))

    def run(self, plan):
        try:
            self.acquire_radio_lock()
            self.start_monitor()
            for candidate in plan:
                try:
                    facts, ssb = self.attempt(candidate, True)
                    if facts is None and ssb is not None:
                        # An edge-of-window SSB can put CORESET0 outside the capture.
                        # Recenter from this run's PBCH evidence, not a fixed SSB position.
                        centered = dict(candidate, center_hz=ssb['ss_ref_hz'],
                                        ssb_sc=6 * candidate['prb'] - 120)
                        facts, ssb = self.attempt(centered, True, seed=ssb)
                    if facts is not None:
                        save(self.out / 'candidate.json', facts)
                        self.attempt(facts, False, expected=facts, seed=ssb)
                        return
                except Unsupported as error:
                    self.state('UNSUPPORTED', reason=str(error), hypothesis=candidate)
            self.state('NOT_ACQUIRED', reason='Finite search exhausted; no supported broadcast-validated carrier')
        except KeyboardInterrupt:
            self.state('STOPPED')
        except Exception as error:
            self.state('VOID', reason=str(error))
            raise
        finally:
            try:
                self.stop(self.child)
            finally:
                # Leave this run's dashboard available for diagnosis. It has
                # its own session, no radio lock, and marks old counters stale.
                if self.lock_fd is not None:
                    os.close(self.lock_fd)
                self.runlog.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--start-hz', required=True, type=int, help='Lower SSB search bound, not a carrier hint')
    parser.add_argument('--stop-hz', required=True, type=int, help='Upper SSB search bound')
    parser.add_argument('--usrp-args', required=True, help='Receiver hardware addresses only')
    parser.add_argument('--out', required=True, type=Path, help='New capture directory')
    parser.add_argument('--binary', type=Path, default=REPO / 'cmake_targets/ran_build/build/nr-uesoftmodem')
    parser.add_argument('--interface', default='enp129s0f0np0')
    parser.add_argument('--rx-gain', type=float, default=40, help='Receiver front-end gain, not a decoded cell parameter')
    parser.add_argument('--max-bandwidth-mhz', type=int, choices=BW_MHZ, default=100)
    parser.add_argument('--cfo-max-hz', type=int, default=200000, help='Symmetric CFO search bound, not a CFO seed')
    parser.add_argument('--dwell-seconds', type=float, default=180)
    parser.add_argument('--duration-seconds', type=float, default=0, help='Seconds after first broadcast-validated lock; 0 means continuous')
    parser.add_argument('--monitor-port', type=int, default=8081)
    parser.add_argument('--monitor-bind', default='127.0.0.1',
                        help='HTTP bind address; use a LAN address only when explicitly wanted')
    args = parser.parse_args()
    if not 410000000 <= args.start_hz < args.stop_hz <= 7125000000:
        parser.error('Supply ordered FR1 search limits between 410 MHz and 7.125 GHz')
    if not 0 < args.cfo_max_hz <= 32 * 15000:
        parser.error('CFO bound must be between 1 and 480000 Hz')
    if not math.isfinite(args.dwell_seconds) or args.dwell_seconds <= 0:
        parser.error('Dwell must be finite and positive')
    if not math.isfinite(args.duration_seconds) or args.duration_seconds < 0:
        parser.error('Duration must be finite and nonnegative')
    if not math.isfinite(args.rx_gain) or not 1 <= args.monitor_port <= 65535:
        parser.error('Invalid RF gain or monitor port')
    if os.geteuid() != 0:
        parser.error('Run with sudo for the existing real-time receiver and affinity configuration')
    args.binary = args.binary.resolve(strict=True)
    rows, bandwidths = standard_tables()
    plan = scan_plan(args, rows, bandwidths)
    if not plan:
        parser.error('No supported FR1 raster points inside these search limits')
    signal.signal(signal.SIGTERM, lambda *_: (_ for _ in ()).throw(KeyboardInterrupt()))
    acquisition = Acquisition(args, rows, bandwidths)
    save(acquisition.out / 'scan_plan.json', plan)
    acquisition.run(plan)


if __name__ == '__main__':
    main()
