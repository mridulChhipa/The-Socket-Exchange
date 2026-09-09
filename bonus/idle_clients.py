#!/usr/bin/env python3
"""
idle_clients.py -- Bonus (Section 6.9): client-generation program

Opens N TCP connections to the Exchange Server and keeps them ESTABLISHED
without exchanging any application-level data. Prints progress as it goes
and periodic liveness reports while holding connections open.

Usage:
    python3 idle_clients.py <host> <port> <n> [options]

Examples:
    # open 10,000 idle connections to a server on localhost:5000
    python3 idle_clients.py 127.0.0.1 5000 10000

    # open 70,000 by round-robining across 4 source IPs from 127.0.0.0/8
    # (each source IP unlocks another ~55k ephemeral ports on loopback)
    python3 idle_clients.py 127.0.0.1 5000 70000 --batch 2000 \
                            --src-cidr 127.0.0.0/8 --src-count 4

    # or explicitly list the source IPs to rotate over
    python3 idle_clients.py 127.0.0.1 5000 70000 --batch 2000 \
                            --src-ips 127.0.0.1,127.0.0.2

Options:
    --batch B        progress log every B successful connects (default 500)
    --sleep S        sleep S seconds between connects (default 0)
    --report-sec T   liveness report every T seconds after all connects done (default 30)
    --no-raise-fd    do not attempt to raise RLIMIT_NOFILE
    --src-ips IPS    comma-separated source IPs to round-robin over
    --src-cidr CIDR  CIDR to auto-generate the source-IP pool (e.g. 127.0.0.0/8)
    --src-count K    how many IPs to draw from --src-cidr (default 4)

When neither --src-ips nor --src-cidr is given, the script behaves as
before: the kernel picks the source IP (127.0.0.1) and the source port.
The 4-tuple constraint then caps a single-process run at ~55 000
connections against one (dst_ip, dst_port). Rotation across N source IPs
raises that ceiling to ~N x 55 000.

Stop the script with Ctrl-C to close all sockets and exit.
"""

import argparse
import ipaddress
import os
import resource
import signal
import socket
import sys
import time


def bump_fd_limit(target):
    """Try to raise RLIMIT_NOFILE so we can hold `target` open FDs (+ headroom)."""
    soft, hard = resource.getrlimit(resource.RLIMIT_NOFILE)
    want = target + 128
    if soft >= want:
        return soft, hard
    new_hard = max(hard, want)
    try:
        resource.setrlimit(resource.RLIMIT_NOFILE, (want, new_hard))
        soft2, hard2 = resource.getrlimit(resource.RLIMIT_NOFILE)
        return soft2, hard2
    except (ValueError, OSError) as e:
        print(f"[warn] could not raise RLIMIT_NOFILE to {want}: {e}", file=sys.stderr)
        print(f"[warn] current soft={soft} hard={hard}", file=sys.stderr)
        print("[warn] Try: (as root) sysctl kern.maxfiles=... kern.maxfilesperproc=...", file=sys.stderr)
        print("[warn]      or run this script under `sudo` / adjust login.conf.", file=sys.stderr)
        return soft, hard


def print_env_hints():
    print("[i] FreeBSD tuning hints if you hit failures at large N:")
    print("    sysctl kern.maxfiles kern.maxfilesperproc kern.ipc.somaxconn")
    print("    sysctl net.inet.ip.portrange.first net.inet.ip.portrange.last")
    print("    sysctl kern.ipc.maxsockbuf net.inet.tcp.sendspace net.inet.tcp.recvspace")
    print("    (raise them via /etc/sysctl.conf or `sudo sysctl KEY=VAL` before running)")


def main():
    ap = argparse.ArgumentParser(
        description="Open N idle TCP connections to a server and hold them open.")
    ap.add_argument("host", help="server host, e.g. 127.0.0.1")
    ap.add_argument("port", type=int, help="server TCP port, e.g. 5000")
    ap.add_argument("n", type=int, help="number of idle connections to open")
    ap.add_argument("--batch", type=int, default=500,
                    help="log progress every B successful connects (default 500)")
    ap.add_argument("--sleep", type=float, default=0.0,
                    help="seconds to sleep between connects (default 0)")
    ap.add_argument("--report-sec", type=int, default=30,
                    help="liveness report interval after all connects done (default 30)")
    ap.add_argument("--no-raise-fd", action="store_true",
                    help="do not attempt to raise RLIMIT_NOFILE")
    ap.add_argument("--src-ips", default=None,
                    help="comma-separated source IPs to round-robin, "
                         "e.g. 127.0.0.1,127.0.0.2")
    ap.add_argument("--src-cidr", default=None,
                    help="CIDR to auto-generate the source-IP pool, "
                         "e.g. 127.0.0.0/8")
    ap.add_argument("--src-count", type=int, default=4,
                    help="how many IPs to draw from --src-cidr (default 4)")
    args = ap.parse_args()

    if args.n <= 0:
        print("n must be positive", file=sys.stderr)
        sys.exit(2)

    # Build the source-IP rotation pool if either option was supplied.
    # If both --src-ips and --src-cidr are given, --src-ips wins.
    src_ips = None
    if args.src_ips:
        src_ips = [ip.strip() for ip in args.src_ips.split(",") if ip.strip()]
    elif args.src_cidr:
        try:
            net = ipaddress.ip_network(args.src_cidr, strict=False)
        except ValueError as e:
            print(f"[!] invalid --src-cidr: {e}", file=sys.stderr)
            sys.exit(2)
        src_ips = []
        for i, host in enumerate(net.hosts()):
            if i >= args.src_count:
                break
            src_ips.append(str(host))
    if src_ips is not None and len(src_ips) == 0:
        print("[!] source-IP pool is empty; ignoring --src-ips/--src-cidr",
              file=sys.stderr)
        src_ips = None

    print(f"[+] target: {args.n} idle TCP connections -> {args.host}:{args.port}")
    print(f"[+] client PID = {os.getpid()}  (use for `procstat -f {os.getpid()}` etc.)")
    if src_ips is not None:
        print(f"[+] rotating source IPs over pool of {len(src_ips)}: "
              f"{', '.join(src_ips)}")
        print(f"[+] theoretical single-run ceiling raised to "
              f"~{len(src_ips) * 55000:,} connections")
    else:
        print("[+] source IP left to kernel (single source, ~55k ceiling on loopback)")

    if not args.no_raise_fd:
        soft, hard = bump_fd_limit(args.n)
        print(f"[+] RLIMIT_NOFILE now soft={soft} hard={hard}")

    print_env_hints()

    socks = []
    failures = 0
    first_fail_index = None
    t0 = time.time()

    # Ignore SIGPIPE: server-side closes shouldn't kill us via a write we never do.
    try:
        signal.signal(signal.SIGPIPE, signal.SIG_IGN)
    except (AttributeError, ValueError):
        pass

    try:
        for i in range(args.n):
            s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            # We never send/recv; disabling Nagle isn't needed. Keep defaults.
            try:
                if src_ips is not None:
                    # Round-robin the source IP so each src_ip has its own
                    # ephemeral-port space (~55k on loopback). Port 0 lets
                    # the kernel pick an unused ephemeral port for that src.
                    s.bind((src_ips[i % len(src_ips)], 0))
                s.connect((args.host, args.port))
                socks.append(s)
            except OSError as e:
                failures += 1
                if first_fail_index is None:
                    first_fail_index = i
                s.close()
                if failures <= 5:
                    print(f"[!] connect #{i} failed: {e.errno} {e.strerror}",
                          file=sys.stderr)
                # bail out if we get a run of failures
                if failures >= 20 and failures > (i + 1) * 0.5:
                    print("[!] too many failures, aborting connect loop", file=sys.stderr)
                    break

            if (i + 1) % args.batch == 0:
                elapsed = time.time() - t0
                rate = (i + 1) / elapsed if elapsed > 0 else 0.0
                print(f"    established={len(socks):>7d}  failed={failures:>5d}  "
                      f"elapsed={elapsed:6.1f}s  rate={rate:7.0f}/s")

            if args.sleep > 0:
                time.sleep(args.sleep)
    except KeyboardInterrupt:
        print("\n[!] interrupted during connect loop", file=sys.stderr)

    elapsed = time.time() - t0
    print()
    print(f"[+] connect phase done: established={len(socks)}  failed={failures}  "
          f"elapsed={elapsed:.1f}s")
    if first_fail_index is not None:
        print(f"[+] first failure was at attempt #{first_fail_index}")
    print(f"[+] client PID = {os.getpid()}")
    print(f"[+] holding {len(socks)} sockets open. Ctrl-C to close all and exit.")
    print()
    print("    While this holds, in another terminal you can run e.g.:")
    print(f"      sockstat -4 -c | wc -l")
    print(f"      netstat -an -p tcp | awk '$6==\"ESTABLISHED\"' | wc -l")
    print(f"      procstat -f <server-pid> | wc -l")
    print(f"      ps -o pid,rss,vsz,%cpu -p <server-pid>")
    print(f"      netstat -m")
    print(f"      sysctl kern.openfiles kern.maxfiles")

    # Hold. Periodically report how many sockets still appear open on our side.
    try:
        while True:
            time.sleep(args.report_sec)
            alive = sum(1 for s in socks if s.fileno() >= 0)
            print(f"[.] holding: sockets_alive={alive} pid={os.getpid()} "
                  f"uptime={time.time()-t0:.0f}s")
    except KeyboardInterrupt:
        print("\n[+] closing sockets ...")
        for s in socks:
            try:
                s.close()
            except OSError:
                pass
        print("[+] done.")


if __name__ == "__main__":
    main()
