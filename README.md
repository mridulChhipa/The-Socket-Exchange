# Assignment 2: The Socket Exchange

## Team

- Student 1: `Mridul Chhipa (2024CS10411)`
- Student 2: `Harsh Verma (2024CS10499)`

## Overview

This project implements the Exchange Server, Trader Client, and Market-Data Client
for COL334 Assignment 2. All communication uses TCP sockets and newline-delimited
text messages.

## Requirements

- Operating system: FreeBSD 14.4-RELEASE or later
- Language: C
- Compiler: `<compiler and version>`
- Runtime/dependencies: `<list any required packages or write "None">`

The server and clients use the POSIX socket API directly. No third-party networking
frameworks are required.

## Build

From the submission root, run:

```sh
make
```

This produces:

- `exchange_server`
- `trader_client`
- `market_data_client`

To remove compiled files:

```sh
make clean
```

## Run

Use the launcher scripts in `server/` and `client/`. They pass every argument
through to the underlying binary, so the caller does not need to know the
implementation language.

Start the Exchange Server:

```sh
./server/run-server [bind_address] [port]
```

Start a Trader Client:

```sh
./client/run-trader [host] [port] [username]
```

Start a Market-Data Client:

```sh
./client/run-market-data [host] [port] [instrument]
```

For example:

```sh
./server/run-server 127.0.0.1 5000
./client/run-trader 127.0.0.1 5000 alice
./client/run-market-data 127.0.0.1 5000 JNST
```

All arguments are optional. With none given the server listens on every
interface on port 8080, and the clients connect to `127.0.0.1:8080`. If a
Trader Client is given a username it sends `LOGIN <username>` on connecting;
otherwise commands are read from standard input, one per line.

## Test

With the server running:

```sh
python3 testing/test_framing.py [host] [port]   # server-side message framing
python3 testing/test_client.py                  # drives ./trader_client itself
```

Both exit non-zero if any check fails.

## Assumptions
- Usernames are at most 127 characters. This is enforced and diagnosed rather than
  assumed: a longer name is rejected with `ERROR Username too long`, never truncated.
- Integer values (quantity, price, order ID) fit in a 32-bit signed int, so at most
  10 digits.
- Using epoll / kqueue for handling concurrent connections and multiplexing.
- Using non-blocking sockets, otherwise the final recv() call would freeze the entire server waiting for data that hasn't arrived yet.

## Implementation Notes

- Concurrency/I/O mechanism: a single-threaded event loop over non-blocking sockets.
  The listening socket is level-triggered; client sockets are edge-triggered, so each
  readable socket is drained until `EAGAIN`.
- Message-framing approach: a fixed per-connection line buffer sized from the protocol.
  See *Bounded-line message framing* below.
- Order-book/matching approach: `<briefly describe>`
- Connection-error handling: `SIGPIPE` is ignored process-wide, so a client that
  disappears mid-write fails the `write()` with `EPIPE` instead of terminating the
  server. A connection is reaped when `read()` returns 0 (peer sent FIN) or fails with
  anything other than `EINTR`/`EAGAIN`; its slot is freed and the descriptor removed
  from the event set.

## Design Decisions

### Bounded-line message framing

TCP is a byte stream, so a `read()` may return part of a message, several messages, or
a message split at any position. Each connection therefore owns a buffer that persists
across reads; complete lines are extracted from it and the incomplete tail is kept for
the next read.

**The buffer is sized from the protocol, not guessed.** The command set is closed and
every token has a known maximum length, so the longest legal message is computable:

| Message | Longest legal form | Bytes |
| --- | --- | --- |
| `LOGIN <username>` | `LOGIN` + ` ` + 127-char name | **133** |
| `SELL <inst> <qty> <price>` | 4+1+4+1+10+1+10 | 31 |
| `BUY <inst> <qty> <price>` | 3+1+4+1+10+1+10 | 30 |
| `CANCEL <order_id>` | 6+1+10 | 17 |
| `UNSUBSCRIBE <inst>` | 11+1+4 | 16 |
| `SUBSCRIBE <inst>` | 9+1+4 | 14 |
| `QUIT` | 4 | 4 |

Instruments are 4 characters; integers are at most 10 digits (`INT_MAX` is
2147483647); the longest command word is `UNSUBSCRIBE` at 11. `LOGIN` dominates, so
`MAX_LINE_LEN` is defined as `MAX_CMD_LEN + 1 + (USERNAME_LENGTH - 1) + 1` = 140 bytes.
Nothing is hardcoded: changing `USERNAME_LENGTH` resizes the buffer correctly.

**The buffer is 160 bytes, deliberately larger than 140.** The extra 20 bytes are a
diagnostic window, not slack. A line that is illegal but still fits is buffered whole,
so the parser can identify *which* token is oversized and answer precisely:

| Condition | Response |
| --- | --- |
| Command word longer than `UNSUBSCRIBE` | `ERROR Unknown command` |
| `LOGIN` argument over 127 characters | `ERROR Username too long` |
| Line too long to buffer at all | `ERROR Message too long` |

Without the headroom every one of these would collapse into the generic
`ERROR Message too long`. The boundary is worth stating: a username of 128–153
characters gets the precise error; 154 or more overflows the buffer and gets the
generic one. Both are rejected — only the diagnosis differs.

**Tokens are captured at full width and length-checked afterwards.** A narrow `scanf`
width cannot detect an over-long token, it silently truncates: `%11s` applied to
`UNSUBSCRIBEXX JNST` yields `UNSUBSCRIBE` plus `XX` and looks like a valid two-token
command. Capturing at full width and then comparing lengths is what makes the errors
above possible, and it means a username can never be silently cut to 127 characters
and accepted as if it were the name the client sent.

**An over-long line does not kill the connection.** Because the buffer is exactly one
maximum line plus headroom, a full buffer containing no newline is not a resource
shortage — it is proof that the client sent something no conforming client could send.
The server reports it once and then discards bytes until the next newline, at which
point normal parsing resumes. Anything following that newline in the same segment is
processed normally, and the `report once` guard stops a multi-megabyte garbage line
from generating thousands of error replies.

**Scanning is linear in the message length.** Each connection records how many leading
bytes have already been searched for a newline, so a message arriving in *k* segments
costs O(length) in total rather than O(length x k) — relevant because the framing
experiment delivers one message in several small pieces.

**Effect on scalability.** At 160 bytes per connection instead of a round 1 KB, 70,000
idle connections hold roughly 11 MB of input buffers rather than about 72 MB. Note that
kernel socket buffers dominate per-connection memory at that scale; the line buffer is
the portion the application controls, which makes it worth measuring both ways in the
scalability experiment.

## Bonus (Section 6.9) — running the scalability experiment

The two helper programs required by the bonus live in `bonus/`:

- `bonus/idle_clients.py` — client-generation program. Opens N TCP connections to
  the Exchange Server and holds them ESTABLISHED without exchanging any application
  data. Prints progress and a summary of failures.
- `bonus/measure.sh` — one-shot snapshot of the server's resource usage (RSS, %CPU,
  FDs, mbuf clusters, socket-buffer sysctls, kernel stack). Meant to be run while
  `idle_clients.py` is holding the connections open.

Full tuning recipe, per-N raw outputs, and the analysis are in `bonus/README.md`
and in the report appendix. Below are only the commands needed to reproduce the
runs.

### Prerequisite: OS tuning (once, as root on FreeBSD 14)

`bonus/README.md` §3–§5 has the full explanation. In brief, before the first run:

```sh
# 1. Boot-time tunables (edit /boot/loader.conf, then reboot):
#      kern.ipc.maxsockets=400000
#      kern.ipc.nmbclusters=500000

# 2. Runtime sysctls (persist in /etc/sysctl.conf; apply live with):
sysctl kern.ipc.somaxconn=65535
sysctl kern.maxfiles=300000 kern.maxfilesperproc=250000
sysctl net.inet.ip.portrange.first=10000 net.inet.ip.portrange.last=65535

# 3. Extra loopback addresses (only needed for N > 55 000):
ifconfig lo0 alias 127.0.0.2/32
ifconfig lo0 alias 127.0.0.3/32
ifconfig lo0 alias 127.0.0.4/32

# 4. In every shell that launches the server or the generator:
ulimit -n 250000
```

### Running the experiment (three terminals)

**Terminal 1 — server:**

```sh
ulimit -n 250000
./server/run-server 127.0.0.1 5000
```

**Terminal 2 — client generator** (from the submission root):

```sh
ulimit -n 250000

# 10 000 / 20 000 / 30 000 / 40 000 / 50 000: single source IP is enough.
python3 bonus/idle_clients.py 127.0.0.1 5000 10000 --batch 2000
python3 bonus/idle_clients.py 127.0.0.1 5000 50000 --batch 2000

# 60 000 / 70 000: rotate across four 127.x.x.x source IPs to bypass the
# ~55 000 single-source-IP ephemeral-port ceiling (TCP 4-tuple limit).
python3 bonus/idle_clients.py 127.0.0.1 5000 70000 --batch 2000 \
        --src-cidr 127.0.0.0/8 --src-count 4
```

Leave the generator running; it holds every socket open until you Ctrl-C it.

**Terminal 3 — measurement snapshot** (run while Terminal 2 is holding connections):

```sh
SPID=$(pgrep -f exchange_server | head -1)   # or read it from Terminal 1's log
./bonus/measure.sh $SPID 5000 | tee run_70k.txt
```

Repeat for each N; save each output as `run_<N>.txt`. That single file supplies
every column of the Section 6.9 table (RSS/CPU from `ps`, server FDs from
`procstat -f`, system-wide FDs from `sysctl kern.openfiles`, socket-buffer usage
from `netstat -m`, established count cross-checked via `sockstat`, and the
kernel stack of the server thread from `procstat -k`).

### `idle_clients.py` command-line reference

```
python3 bonus/idle_clients.py <host> <port> <n> [options]

  --batch B        progress log every B successful connects (default 500)
  --sleep S        sleep S seconds between connects (default 0)
  --report-sec T   liveness report every T seconds after all connects done (default 30)
  --no-raise-fd    do not attempt to raise RLIMIT_NOFILE
  --src-ips IPS    comma-separated source IPs to round-robin over
  --src-cidr CIDR  CIDR to auto-generate the source-IP pool (e.g. 127.0.0.0/8)
  --src-count K    how many IPs to draw from --src-cidr (default 4)
```

Default behaviour (no `--src-*` flags) is a single-source-IP client; the
`--src-*` flags are additive and used only for `N > 55 000`.
