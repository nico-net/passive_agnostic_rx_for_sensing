#!/usr/bin/env python3
"""Claim/exec and pidfd signaling helper for run_ocudu_passive.sh."""

import argparse
import fcntl
import os
import signal
import stat
import sys
import tempfile


def starttime(pid):
    with open(f"/proc/{pid}/stat", "r", encoding="ascii") as stat_file:
        stat = stat_file.read()
    close = stat.rfind(")")
    if close < 0:
        raise RuntimeError(f"malformed /proc/{pid}/stat")
    fields = stat[close + 2 :].split()  # starts at field 3; starttime is field 22
    return int(fields[19])


def write_claim(path, values):
    directory = os.path.dirname(os.path.abspath(path))
    fd, temporary = tempfile.mkstemp(prefix=".claim-", dir=directory)
    try:
        os.fchmod(fd, 0o644)
        with os.fdopen(fd, "w", encoding="ascii") as claim:
            claim.write(" ".join(str(value) for value in values) + "\n")
            claim.flush()
            os.fsync(claim.fileno())
        os.replace(temporary, path)
        dir_fd = os.open(directory, os.O_RDONLY | getattr(os, "O_DIRECTORY", 0))
        try:
            os.fsync(dir_fd)
        finally:
            os.close(dir_fd)
    finally:
        try:
            os.unlink(temporary)
        except FileNotFoundError:
            pass


def registry_append(path, text, already_locked=False):
    directory = os.path.dirname(os.path.abspath(path))
    lock_fd = None if already_locked else os.open(path + ".lock", os.O_CREAT | os.O_RDWR, 0o600)
    try:
        if lock_fd is not None:
            fcntl.flock(lock_fd, fcntl.LOCK_EX)
        current_stat = os.stat(path, follow_symlinks=False)
        if not stat.S_ISREG(current_stat.st_mode):
            raise RuntimeError("registry is not a regular file")
        with open(path, "rb") as registry:
            previous = registry.read()
        addition = text.encode("ascii")
        if not addition.endswith(b"\n"):
            addition += b"\n"
        fd, temporary = tempfile.mkstemp(prefix=".pids-", dir=directory)
        try:
            os.fchmod(fd, stat.S_IMODE(current_stat.st_mode))
            with os.fdopen(fd, "wb") as registry:
                registry.write(previous)
                registry.write(addition)
                registry.flush()
                os.fsync(registry.fileno())
            os.replace(temporary, path)
            dir_fd = os.open(directory, os.O_RDONLY | getattr(os, "O_DIRECTORY", 0))
            try:
                os.fsync(dir_fd)
            finally:
                os.close(dir_fd)
        finally:
            try:
                os.unlink(temporary)
            except FileNotFoundError:
                pass
    finally:
        if lock_fd is not None:
            fcntl.flock(lock_fd, fcntl.LOCK_UN)
            os.close(lock_fd)


def claim_exec(args):
    if not args.command:
        raise RuntimeError("missing command after --")
    if args.track_parent and not args.monitor_claim:
        raise RuntimeError("--track-parent requires --monitor-claim")
    if args.cwd:
        os.chdir(args.cwd)
    values = [os.getpid(), starttime(os.getpid())]
    if args.track_parent:
        with open(args.monitor_claim, "r", encoding="ascii") as monitor_file:
            monitor_pid = int(monitor_file.read().split()[0])
        if os.getpid() != monitor_pid:
            parent = os.getppid()
            if parent <= 1:
                raise RuntimeError("cannot identify privileged process parent")
            values.extend((parent, starttime(parent)))
    write_claim(args.claim, values)
    if args.stop_after_claim:
        os.kill(os.getpid(), signal.SIGSTOP)
    os.execvpe(args.command[0], args.command, os.environ)


def supervise(args):
    if not args.command:
        raise RuntimeError("missing command after --")
    if args.cwd:
        os.chdir(args.cwd)
    write_claim(args.claim, [os.getpid(), starttime(os.getpid())])
    if args.stop_before_claim:
        os.kill(os.getpid(), signal.SIGSTOP)
    os.execvpe(args.command[0], args.command, os.environ)


def pidfd_signal(args):
    if not hasattr(os, "pidfd_open") or not hasattr(signal, "pidfd_send_signal"):
        raise RuntimeError("pidfd_open/pidfd_send_signal unavailable; refusing PID signaling")
    pid = int(args.pid)
    expected_start = int(args.starttime)
    signum = getattr(signal, args.signal_name)
    pidfd = os.pidfd_open(pid, 0)
    try:
        if starttime(pid) != expected_start:
            raise RuntimeError(f"PID {pid} start-time mismatch; refusing signal")
        signal.pidfd_send_signal(pidfd, signum, None, 0)
    finally:
        os.close(pidfd)


def main():
    parser = argparse.ArgumentParser()
    commands = parser.add_subparsers(dest="operation", required=True)
    claim = commands.add_parser("exec")
    claim.add_argument("--claim", required=True)
    claim.add_argument("--track-parent", action="store_true")
    claim.add_argument("--stop-after-claim", action="store_true")
    claim.add_argument("--monitor-claim")
    claim.add_argument("--cwd")
    claim.add_argument("command", nargs=argparse.REMAINDER)
    supervisor = commands.add_parser("supervise")
    supervisor.add_argument("--claim", required=True)
    supervisor.add_argument("--cwd")
    supervisor.add_argument("--stop-before-claim", action="store_true")
    supervisor.add_argument("command", nargs=argparse.REMAINDER)
    signal_parser = commands.add_parser("signal")
    signal_parser.add_argument("pid")
    signal_parser.add_argument("starttime")
    signal_parser.add_argument("signal_name", choices=("SIGCONT", "SIGINT", "SIGTERM", "SIGKILL"))
    registry_parser = commands.add_parser("registry-append")
    registry_parser.add_argument("--locked", action="store_true")
    registry_parser.add_argument("path")
    registry_parser.add_argument("text")
    args = parser.parse_args()
    try:
        if args.operation == "exec":
            command = args.command
            if command and command[0] == "--":
                command = command[1:]
            args.command = command
            claim_exec(args)
        elif args.operation == "supervise":
            command = args.command
            if command and command[0] == "--":
                command = command[1:]
            args.command = command
            supervise(args)
        elif args.operation == "signal":
            pidfd_signal(args)
        else:
            registry_append(args.path, args.text, args.locked)
    except Exception as error:  # fail closed and keep diagnostics concise
        print(f"ocudu_owned_process: {error}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
