#!/usr/bin/env python3
"""Framing tests for the Exchange Server.

These cannot be done by typing at the trader client: a terminal turns "\\n" into
a backslash and an 'n', so every typed line is exactly one protocol message.
Only a raw socket can put several messages in one TCP segment, or split one
message across several, which is what the framing rules in the specification are
actually about.

Usage:
    ./exchange_server &
    python3 testing/test_framing.py [host] [port]
"""

import socket
import sys
import time

HOST = "127.0.0.1"
PORT = 8080

RESULTS = []


class Conn:
    """A socket that reassembles newline-delimited replies."""

    def __init__(self, host, port, timeout=2.0):
        self.sock = socket.create_connection((host, port), timeout=timeout)
        self.buf = b""

    def send(self, data):
        self.sock.sendall(data.encode() if isinstance(data, str) else data)

    def send_slowly(self, data, delay=0.05):
        """One byte per segment, so the server must reassemble."""
        raw = data.encode() if isinstance(data, str) else data
        for i in range(len(raw)):
            self.sock.sendall(raw[i : i + 1])
            time.sleep(delay)

    def _take_buffered(self, out, count):
        while b"\n" in self.buf and len(out) < count:
            line, self.buf = self.buf.split(b"\n", 1)
            out.append(line.rstrip(b"\r").decode(errors="replace"))

    def lines(self, count, timeout=2.0):
        """Read until `count` complete lines are available, or time out."""
        out = []
        deadline = time.time() + timeout

        while len(out) < count:
            self._take_buffered(out, count)
            if len(out) >= count:
                break

            remaining = deadline - time.time()
            if remaining <= 0:
                break

            self.sock.settimeout(remaining)
            try:
                chunk = self.sock.recv(4096)
            except (socket.timeout, TimeoutError):
                break
            if not chunk:
                break
            self.buf += chunk

        return out

    def drain(self, timeout=0.4):
        """Everything else that arrives in a short window. Should usually be []."""
        out = []
        deadline = time.time() + timeout

        while True:
            self._take_buffered(out, 10_000)

            remaining = deadline - time.time()
            if remaining <= 0:
                break

            self.sock.settimeout(remaining)
            try:
                chunk = self.sock.recv(4096)
            except (socket.timeout, TimeoutError):
                break
            if not chunk:
                break
            self.buf += chunk

        return out

    def close(self):
        try:
            self.sock.close()
        except OSError:
            pass


def check(name, ok, detail=""):
    RESULTS.append(ok)
    print(f"  {'PASS' if ok else 'FAIL'}  {name}")
    if not ok and detail:
        print(f"        {detail}")


# --- request framing: several messages per segment, or one across many -------


def test_two_commands_one_segment(host, port):
    """The case a terminal cannot produce: two messages, one write."""
    c = Conn(host, port)
    c.send("LOGIN alice\nQUIT\n")
    got = c.lines(2)
    check(
        "two commands in one segment -> two replies",
        got == ["OK", "OK"],
        f"expected ['OK', 'OK'], got {got}",
    )
    c.close()


def test_many_commands_one_segment(host, port):
    """N requests in one segment must produce exactly N replies, in order."""
    n = 20
    c = Conn(host, port)
    c.send("".join(f"LOGIN user{i}\n" for i in range(n)))
    got = c.lines(n)
    expected = ["OK"] + ["ERROR Already logged in"] * (n - 1)
    check(
        f"{n} commands in one segment -> {n} replies",
        got == expected,
        f"got {len(got)} replies: {got[:4]}...",
    )
    c.close()


def test_message_split_across_segments(host, port):
    """Experiment 3: one message delivered in several pieces."""
    c = Conn(host, port)
    for piece in ("LOG", "IN ", "bo", "b\n"):
        c.send(piece)
        time.sleep(0.2)
    got = c.lines(1)
    check(
        "one message split across 4 segments -> one reply",
        got == ["OK"],
        f"expected ['OK'], got {got}",
    )
    c.close()


def test_message_one_byte_at_a_time(host, port):
    c = Conn(host, port)
    c.send_slowly("LOGIN carol\n")
    got = c.lines(1)
    check(
        "one message, one byte per segment -> one reply",
        got == ["OK"],
        f"expected ['OK'], got {got}",
    )
    c.close()


def test_split_at_the_newline(host, port):
    """The terminator arriving alone is the nastiest split point."""
    c = Conn(host, port)
    c.send("LOGIN dave")
    time.sleep(0.3)
    check("no reply before the newline arrives", c.drain(0.3) == [])
    c.send("\n")
    got = c.lines(1)
    check(
        "message completed by a lone newline -> one reply",
        got == ["OK"],
        f"expected ['OK'], got {got}",
    )
    c.close()


def test_blank_lines_ignored(host, port):
    c = Conn(host, port)
    c.send("\n\n\nLOGIN erin\n")
    got = c.lines(1)
    extra = c.drain()
    check(
        "blank lines ignored, no spurious replies",
        got == ["OK"] and extra == [],
        f"got {got}, extra {extra}",
    )
    c.close()


def test_crlf_tolerated(host, port):
    c = Conn(host, port)
    c.send("LOGIN frank\r\n")
    got = c.lines(1)
    check("CRLF line ending accepted", got == ["OK"], f"got {got}")
    c.close()


# --- over-long lines: diagnosis, then resynchronisation ----------------------


def test_username_too_long_is_diagnosed(host, port):
    """Inside the buffer's headroom, the offending token is named."""
    c = Conn(host, port)
    c.send("LOGIN " + "a" * 140 + "\n")
    got = c.lines(1)
    check(
        "over-long username -> specific error",
        got == ["ERROR Username too long"],
        f"got {got}",
    )
    c.close()


def test_command_word_too_long(host, port):
    c = Conn(host, port)
    c.send("UNSUBSCRIBEXXXXX JNST\n")
    got = c.lines(1)
    check(
        "over-long command word -> Unknown command, not a bogus 2-token match",
        got == ["ERROR Unknown command"],
        f"got {got}",
    )
    c.close()


def test_over_long_line_reports_once_then_resyncs(host, port):
    """A huge line must give exactly one error, and not desync the parser."""
    c = Conn(host, port)
    c.send("LOGIN " + "a" * 500 + "\nQUIT\n")
    got = c.lines(2)
    check(
        "over-long line -> one error, then the next command still works",
        got == ["ERROR Message too long", "OK"],
        f"expected ['ERROR Message too long', 'OK'], got {got}",
    )
    c.close()


def test_over_long_line_does_not_spam(host, port):
    """The report-once guard: 100 KB of garbage is still one error."""
    c = Conn(host, port)
    c.send("LOGIN " + "a" * 100_000 + "\nQUIT\n")
    got = c.lines(2)
    errors = [line for line in got if line.startswith("ERROR")]
    check(
        "100 KB garbage line -> exactly one error reply",
        len(errors) == 1 and got[-1] == "OK",
        f"got {len(errors)} errors in {got}",
    )
    c.close()


# --- connection behaviour ----------------------------------------------------


def test_quit_without_login(host, port):
    """QUIT is valid for both client types and needs no login."""
    c = Conn(host, port)
    c.send("QUIT\n")
    got = c.lines(1)
    check("QUIT without LOGIN -> OK", got == ["OK"], f"got {got}")
    c.close()


def test_server_closes_after_quit(host, port):
    c = Conn(host, port)
    c.send("LOGIN grace\nQUIT\n")
    c.lines(2)
    c.sock.settimeout(2.0)
    try:
        closed = c.sock.recv(1) == b""
    except (socket.timeout, TimeoutError):
        closed = False
    check("server closes the connection after QUIT", closed)
    c.close()


def test_duplicate_username_rejected(host, port):
    a = Conn(host, port)
    a.send("LOGIN heidi\n")
    a.lines(1)

    b = Conn(host, port)
    b.send("LOGIN heidi\n")
    got = b.lines(1)
    check(
        "duplicate username rejected while the first is connected",
        got == ["ERROR Username already in use"],
        f"got {got}",
    )
    a.close()
    b.close()


def test_idle_client_does_not_block_others(host, port):
    """Experiment 4 in miniature: an idle connection must not stall the server."""
    idle = Conn(host, port)  # connects, then says nothing at all
    time.sleep(0.2)

    c = Conn(host, port)
    c.send("LOGIN ivan\n")
    got = c.lines(1)
    check("idle client does not stall another client", got == ["OK"], f"got {got}")

    c.close()
    idle.close()


TESTS = [
    test_two_commands_one_segment,
    test_many_commands_one_segment,
    test_message_split_across_segments,
    test_message_one_byte_at_a_time,
    test_split_at_the_newline,
    test_blank_lines_ignored,
    test_crlf_tolerated,
    test_username_too_long_is_diagnosed,
    test_command_word_too_long,
    test_over_long_line_reports_once_then_resyncs,
    test_over_long_line_does_not_spam,
    test_quit_without_login,
    test_server_closes_after_quit,
    test_duplicate_username_rejected,
    test_idle_client_does_not_block_others,
]


def main():
    host = sys.argv[1] if len(sys.argv) > 1 else HOST
    port = int(sys.argv[2]) if len(sys.argv) > 2 else PORT

    print(f"Testing exchange server at {host}:{port}\n")

    try:
        Conn(host, port).close()
    except OSError as exc:
        print(f"Cannot reach the server at {host}:{port}: {exc}")
        print("Start it first:  ./exchange_server")
        return 2

    for test in TESTS:
        try:
            test(host, port)
        except (OSError, AssertionError) as exc:
            check(test.__name__, False, f"raised {type(exc).__name__}: {exc}")
        time.sleep(0.1)

    passed = sum(RESULTS)
    total = len(RESULTS)
    print(f"\n{passed}/{total} checks passed")

    return 0 if passed == total else 1


if __name__ == "__main__":
    sys.exit(main())