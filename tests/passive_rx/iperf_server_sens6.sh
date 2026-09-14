#!/usr/bin/env bash
# Standalone iperf3 server(s) on sens6, for UEs to connect to directly (not tied to any
# rfsim/open5gs test harness, not tied to the radio lock -- safe to run alongside a live
# X410 capture or anything else on this host).
#
# One iperf3 -s process handles ONE client test at a time -- if a second client connects
# mid-test it gets "server is busy". So for N UEs generating traffic concurrently, this
# starts N server instances on N consecutive ports (default 4 ports from 5201), each
# logged separately. A single UE wanting max single-session throughput should still use
# -P/--parallel on the CLIENT side against one port; the server handles that within one
# session automatically.
#
# Usage:
#   ./iperf_server_sens6.sh start [n_servers] [base_port]
#   ./iperf_server_sens6.sh stop
#   ./iperf_server_sens6.sh status
set -euo pipefail

N="${2:-4}"
BASE_PORT="${3:-5201}"
STATE_DIR="/tmp/iperf_server_sens6"
mkdir -p "$STATE_DIR"

usage() { echo "Usage: $0 {start|stop|status} [n_servers] [base_port]"; exit 1; }

start() {
  [[ $N =~ ^[0-9]+$ && $N -ge 1 ]] || { echo "n_servers must be a positive integer"; exit 1; }
  [[ $BASE_PORT =~ ^[0-9]+$ ]] || { echo "base_port must be numeric"; exit 1; }
  for ((i = 0; i < N; i++)); do
    port=$((BASE_PORT + i))
    pidfile="$STATE_DIR/iperf3.$port.pid"
    if [[ -f $pidfile ]] && kill -0 "$(cat "$pidfile")" 2>/dev/null; then
      echo "port $port: already running (pid $(cat "$pidfile")), skipping"
      continue
    fi
    if ss -tlnH "sport = :$port" 2>/dev/null | grep -q .; then
      echo "port $port: already bound by something else, skipping"
      continue
    fi
    nohup iperf3 -s -p "$port" > "$STATE_DIR/iperf3.$port.log" 2>&1 &
    echo $! > "$pidfile"
    echo "port $port: started (pid $!), log $STATE_DIR/iperf3.$port.log"
  done
  echo
  echo "Reachable addresses on sens6 (pick the one your UE's PDU session actually routes to):"
  ip -4 addr show 2>/dev/null | awk '/inet /{print "  " $2}'
  echo
  echo "Client examples, high-bandwidth (run ON the UE):"
  echo "  TCP, max throughput, 8 parallel streams, 30s:"
  echo "    iperf3 -c <sens6_ip> -p $BASE_PORT -P 8 -t 30"
  echo "  UDP, fixed high offered rate (iperf3 UDP defaults to 1 Mbit/s if -b is omitted):"
  echo "    iperf3 -c <sens6_ip> -p $BASE_PORT -u -b 500M -t 30"
  echo "  Second UE against the next port so it doesn't wait on the first:"
  echo "    iperf3 -c <sens6_ip> -p $((BASE_PORT + 1)) -P 8 -t 30"
}

stop() {
  shopt -s nullglob
  for pidfile in "$STATE_DIR"/iperf3.*.pid; do
    pid=$(cat "$pidfile")
    port=$(basename "$pidfile" .pid); port=${port#iperf3.}
    if kill -0 "$pid" 2>/dev/null; then
      kill "$pid" 2>/dev/null || true
      echo "port $port: stopped (pid $pid)"
    fi
    rm -f "$pidfile"
  done
}

status() {
  shopt -s nullglob
  local any=0
  for pidfile in "$STATE_DIR"/iperf3.*.pid; do
    any=1
    pid=$(cat "$pidfile")
    port=$(basename "$pidfile" .pid); port=${port#iperf3.}
    if kill -0 "$pid" 2>/dev/null; then
      echo "port $port: running (pid $pid)"
    else
      echo "port $port: dead (stale pidfile)"
    fi
  done
  [[ $any == 1 ]] || echo "no servers started by this script"
}

case "${1:-}" in
  start) start ;;
  stop) stop ;;
  status) status ;;
  *) usage ;;
esac
