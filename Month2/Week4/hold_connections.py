#!/usr/bin/env python3
"""
MONTH 2 / WEEK 4 — Experiment A: hold many connections open, measure the cost.
(Week 4 doc, Parts 1–3 and 7.)

For each N in --counts, this:
  1. measures Pulse and the system (BEFORE),
  2. opens N TCP connections to Pulse and sends NOTHING on them — like Week 1's
     silent `nc` client, N times over. Each one makes Pulse create a worker
     thread that sits asleep in read() waiting for bytes that never come,
  3. waits a moment, measures again (DURING), and counts context switches
     over a short window while everything is idle,
  4. closes all N connections (each worker's read() then returns 0, it logs
     "Client disconnected", closes its fd and its thread ends),
  5. waits, measures again (AFTER) to check everything was cleaned up.

What it measures, all read from Linux's /proc files (Week 1 doc, Part 6):
  threads        /proc/<pid>/status  "Threads:"     -> expect N + 1
  fds            /proc/<pid>/fd      (count entries) -> expect N + 4 (0,1,2 + listen socket)
  VmRSS          /proc/<pid>/status  Pulse's own memory actually in RAM
  VmSize         /proc/<pid>/status  Pulse's RESERVED memory (8 MiB per thread stack)
  KernelStack    /proc/meminfo       the kernel's 16 KiB stack per thread (not in VmRSS)
  MemAvailable   /proc/meminfo       free memory of the whole system: the honest total cost
  ctx switches/s /proc/stat "ctxt"   system-wide context switches per second, while idle

Usage (Pulse already running on --port, log sent to /dev/null):
    python3 Month2/Week4/hold_connections.py --port 8080 --counts 100 500 1000 2000

Safety: refuses more than 3000 connections (the Week 4 doc keeps the tests far
below the ~10,000 open-file limit, Part 3).
"""
import argparse
import os
import socket
import subprocess
import sys
import time

MAX_SAFE = 3000


def find_pulse_pid():
    """PID of the running Pulse — the process named exactly 'pulse' (pgrep -x)."""
    out = subprocess.run(["pgrep", "-x", "pulse"], capture_output=True, text=True).stdout.split()
    if len(out) != 1:
        sys.exit(f"Expected exactly one running 'pulse' process, found {len(out)}: {out}. "
                 f"Start one Pulse (and only one), or pass --pid.")
    return int(out[0])


def read_status(pid, key):
    """One numeric field from /proc/<pid>/status, e.g. 'Threads' or 'VmRSS' (kB)."""
    with open(f"/proc/{pid}/status") as f:
        for line in f:
            if line.startswith(key + ":"):
                return int(line.split()[1])
    return 0


def read_meminfo(key):
    """One field from /proc/meminfo, in kB."""
    with open("/proc/meminfo") as f:
        for line in f:
            if line.startswith(key + ":"):
                return int(line.split()[1])
    return 0


def total_context_switches():
    """System-wide context switches since boot: the 'ctxt' line of /proc/stat."""
    with open("/proc/stat") as f:
        for line in f:
            if line.startswith("ctxt"):
                return int(line.split()[1])
    return 0


def snapshot(pid):
    return {
        "threads": read_status(pid, "Threads"),
        "fds": len(os.listdir(f"/proc/{pid}/fd")),
        "rss_kb": read_status(pid, "VmRSS"),
        "vmsize_kb": read_status(pid, "VmSize"),
        "kstack_kb": read_meminfo("KernelStack"),
        "avail_kb": read_meminfo("MemAvailable"),
    }


def run_one(pid, port, n, hold_seconds):
    before = snapshot(pid)

    conns = []
    for _ in range(n):
        s = socket.create_connection(("localhost", port))
        conns.append(s)          # connected, nothing sent: Pulse's worker waits in read()

    time.sleep(1.0)              # let Pulse accept all of them and start every worker
    during = snapshot(pid)

    # Context switches per second over a quiet window, with all N workers asleep.
    c0, t0 = total_context_switches(), time.time()
    time.sleep(hold_seconds)
    c1, t1 = total_context_switches(), time.time()
    cs_per_sec = (c1 - c0) / (t1 - t0)

    for s in conns:
        s.close()                # Pulse's read() returns 0 -> worker ends, fd closed
    time.sleep(1.5)
    after = snapshot(pid)

    print(f"\n===== N = {n} held connections =====")
    print(f"{'':16}{'BEFORE':>12}{'DURING':>12}{'AFTER':>12}{'DURING-BEFORE':>16}{'per conn':>12}")
    rows = [
        ("threads", "threads", ""),
        ("open fds", "fds", ""),
        ("VmRSS kB", "rss_kb", " kB"),
        ("VmSize kB", "vmsize_kb", " kB"),
        ("KernelStack kB", "kstack_kb", " kB"),
    ]
    for label, key, unit in rows:
        d = during[key] - before[key]
        print(f"{label:16}{before[key]:>12}{during[key]:>12}{after[key]:>12}{d:>16}{d / n:>10.1f}{unit}")
    used = before["avail_kb"] - during["avail_kb"]   # how much free memory the system LOST
    print(f"{'MemAvailable kB':16}{before['avail_kb']:>12}{during['avail_kb']:>12}{after['avail_kb']:>12}"
          f"{-used:>16}{used / n:>10.1f} kB used")
    print(f"system context switches/s while all {n} were idle: {cs_per_sec:.0f}")


def main():
    p = argparse.ArgumentParser(description="Hold N idle connections to Pulse and measure the cost.")
    p.add_argument("--port", type=int, default=8080)
    p.add_argument("--counts", type=int, nargs="+", default=[100, 500, 1000, 2000])
    p.add_argument("--hold", type=float, default=3.0, help="seconds to hold, for the context-switch count")
    p.add_argument("--pid", type=int, help="Pulse's PID (default: found with pgrep -x pulse)")
    args = p.parse_args()

    if max(args.counts) > MAX_SAFE:
        sys.exit(f"Refusing more than {MAX_SAFE} connections (Week 4 doc, Part 3: stay far below the limits).")

    pid = args.pid or find_pulse_pid()
    print(f"Pulse PID {pid}, port {args.port}")
    for n in args.counts:
        run_one(pid, args.port, n, args.hold)


if __name__ == "__main__":
    main()
