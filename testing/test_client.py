#!/usr/bin/env python3
"""Behaviour tests for the trader client.

test_framing.py drives the server with raw sockets and never runs the client, so
it cannot tell whether the client itself frames correctly. This one does the
opposite: it stands up a fake server, launches the real ./trader_client against
it, types on its stdin and inspects both what it puts on the wire and what it
prints.

The fake server binds an ephemeral port and the client is pointed at it, so a
real exchange server may stay running while this suite executes.

Usage:
    python3 testing/test_client.py [path-to-trader_client]
"""

import queue
import socket
import subprocess
import sys
import threading
import time

HOST = "127.0.0.1"
CLIENT = "./trader_client"

RESULTS = []


def check(name, ok, detail=""):
    RESULTS.append(ok)
    print(f"  {'PASS' if ok else 'FAIL'}  {name}")
    if not ok and detail:
        print(f"        {detail}")


class FakeServer:
    """A listening socket that speaks the protocol only as far as a test needs."""

    def __init__(self):
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        # Port 0 lets the OS pick a free one, so these tests never collide with
        # a real exchange server that happens to be running.
        self.sock.bind((HOST, 0))
        self.sock.listen(1)
        self.port = self.sock.getsockname()[1]
        self.conn = None
        self.buf = b""

    def accept(self, timeout=3.0):
        self.sock.settimeout(timeout)
        self.conn, _ = self.sock.accept()
        return self.conn

    def send(self, data):
        self.conn.sendall(data.encode() if isinstance(data, str) else data)

    def lines(self, count, timeout=2.0):
        """Complete newline-delimited messages received from the client."""
        out = []
        deadline = time.time() + timeout

        while len(out) < count:
            while b"\n" in self.buf and len(out) < count:
                line, self.buf = self.buf.split(b"\n", 1)
                out.append(line.rstrip(b"\r").decode(errors="replace"))
            if len(out) >= count:
                break

            remaining = deadline - time.time()
            if remaining <= 0:
                break

            self.conn.settimeout(remaining)
            try:
                chunk = self.conn.recv(4096)
            except (socket.timeout, TimeoutError):
                break
            if not chunk:
                break
            self.buf += chunk

        return out

    def saw_fin(self, timeout=2.0):
        """True if the client shut down its writing side."""
        self.conn.settimeout(timeout)
        try:
            return self.conn.recv(4096) == b""
        except (socket.timeout, TimeoutError):
            return False

    def close(self):
        for sock in (self.conn, self.sock):
            try:
                if sock:
                    sock.close()
            except OSError:
                pass


class ClientProc:
    """The client under test, with its stdout drained by a background thread."""

    def __init__(self, path, host, port):
        self.proc = subprocess.Popen(
            [path, host, str(port)],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            bufsize=1,
        )
        self.queue = queue.Queue()
        threading.Thread(target=self._drain, daemon=True).start()

    def _drain(self):
        for line in self.proc.stdout:
            self.queue.put(line.rstrip("\n"))
        self.queue.put(None)

    def type(self, text):
        self.proc.stdin.write(text)
        self.proc.stdin.flush()

    def close_stdin(self):
        self.proc.stdin.close()

    def output(self, timeout=1.0):
        """Every line printed within a short window."""
        out = []
        deadline = time.time() + timeout

        while True:
            remaining = deadline - time.time()
            if remaining <= 0:
                break
            try:
                line = self.queue.get(timeout=remaining)
            except queue.Empty:
                break
            if line is None:
                break
            out.append(line)

        return out

    def expect(self, needle, timeout=2.0):
        deadline = time.time() + timeout

        while True:
            remaining = deadline - time.time()
            if remaining <= 0:
                return False
            try:
                line = self.queue.get(timeout=remaining)
            except queue.Empty:
                return False
            if line is None:
                return False
            if needle in line:
                return True

    def exited(self, timeout=2.0):
        try:
            self.proc.wait(timeout=timeout)
            return True
        except subprocess.TimeoutExpired:
            return False

    def kill(self):
        try:
            self.proc.kill()
            self.proc.wait(timeout=1)
        except (OSError, subprocess.TimeoutExpired):
            pass


def scenario(path):
    """Fresh fake server plus a freshly launched client, connected.

    The client's startup banner is consumed here so that a test asserting on
    exactly what gets printed does not trip over it.
    """
    srv = FakeServer()
    cli = ClientProc(path, HOST, srv.port)
    srv.accept()
    cli.expect("Connected to server", 3.0)
    return srv, cli


# --- what the client puts on the wire ----------------------------------------


def test_commands_are_newline_terminated(path):
    srv, cli = scenario(path)
    cli.type("LOGIN alice\n")
    got = srv.lines(1)
    check(
        "a typed command reaches the server newline-terminated",
        got == ["LOGIN alice"],
        f"got {got}",
    )
    cli.kill()
    srv.close()


def test_several_commands_at_once(path):
    """Pasting three lines must send three messages without waiting for replies."""
    srv, cli = scenario(path)
    cli.type("LOGIN alice\nLOGIN bob\nQUIT\n")
    got = srv.lines(3)
    check(
        "three commands typed at once -> three framed messages",
        got == ["LOGIN alice", "LOGIN bob", "QUIT"],
        f"got {got}",
    )
    cli.kill()
    srv.close()


def test_command_split_across_stdin_writes(path):
    srv, cli = scenario(path)
    for piece in ("LOG", "IN ", "car"):
        cli.type(piece)
        time.sleep(0.15)
    check("nothing sent before the newline is typed", srv.lines(1, 0.4) == [])
    cli.type("ol\n")
    got = srv.lines(1)
    check(
        "a command typed in pieces is sent as one message",
        got == ["LOGIN carol"],
        f"got {got}",
    )
    cli.kill()
    srv.close()


def test_over_long_input_line(path):
    """An enormous line must be reported and dropped, not split into garbage."""
    srv, cli = scenario(path)
    cli.type("X" * 2000 + "\n")
    reported = cli.expect("too long", 2.0)
    check("over-long typed line is reported", reported)

    cli.type("LOGIN dave\n")
    got = srv.lines(1)
    check(
        "the next command still works after an over-long line",
        got == ["LOGIN dave"],
        f"got {got}",
    )
    cli.kill()
    srv.close()


def test_blank_lines_not_sent(path):
    srv, cli = scenario(path)
    cli.type("\n\n\n")
    check("blank lines are not put on the wire", srv.lines(1, 0.5) == [])
    cli.type("LOGIN erin\n")
    check("a real command after blank lines still arrives", srv.lines(1) == ["LOGIN erin"])
    cli.kill()
    srv.close()


# --- what the client does with what it receives ------------------------------


def test_unsolicited_message_shown_immediately(path):
    """The lock-step bug: a notification with no command pending must still print."""
    srv, cli = scenario(path)
    time.sleep(0.3)
    srv.send("BOUGHT JNST 60 238\n")
    check(
        "server message with no command pending is shown immediately",
        cli.expect("BOUGHT JNST 60 238", 2.0),
        "client is only reading the socket after it sends something",
    )
    cli.kill()
    srv.close()


def test_two_replies_in_one_segment(path):
    srv, cli = scenario(path)
    cli.type("LOGIN frank\n")
    srv.lines(1)
    srv.send("OK\nBOUGHT JNST 5 10\n")
    printed = cli.output(1.5)
    check(
        "two replies in one segment -> both printed",
        any("OK" in line for line in printed)
        and any("BOUGHT JNST 5 10" in line for line in printed),
        f"printed {printed}",
    )
    cli.kill()
    srv.close()


def test_reply_split_across_segments(path):
    srv, cli = scenario(path)
    cli.type("LOGIN grace\n")
    srv.lines(1)
    srv.send("O")
    time.sleep(0.3)
    early = cli.output(0.3)
    check("nothing printed before the reply's newline", early == [], f"printed {early}")
    srv.send("K\n")
    printed = cli.output(1.0)
    check(
        "a reply split across segments prints once, whole",
        len(printed) == 1 and printed[0].endswith("OK"),
        f"printed {printed}",
    )
    cli.kill()
    srv.close()


# --- connection termination ---------------------------------------------------


def test_stdin_eof_half_closes(path):
    srv, cli = scenario(path)
    cli.close_stdin()

    check("stdin EOF -> client sends FIN", srv.saw_fin(2.0))

    srv.send("SOLD JNST 10 5\n")
    check(
        "client keeps reading after half-closing",
        cli.expect("SOLD JNST 10 5", 2.0),
        "client exited instead of draining the server's remaining messages",
    )

    srv.conn.close()
    check("client exits once the server closes", cli.exited(2.0))
    srv.close()


def test_client_exits_when_server_closes(path):
    srv, cli = scenario(path)
    cli.type("LOGIN heidi\n")
    srv.lines(1)
    srv.send("OK\n")
    cli.expect("OK", 1.0)
    srv.conn.close()
    check("client exits when the server closes the connection", cli.exited(2.0))
    cli.kill()
    srv.close()


TESTS = [
    test_commands_are_newline_terminated,
    test_several_commands_at_once,
    test_command_split_across_stdin_writes,
    test_over_long_input_line,
    test_blank_lines_not_sent,
    test_unsolicited_message_shown_immediately,
    test_two_replies_in_one_segment,
    test_reply_split_across_segments,
    test_stdin_eof_half_closes,
    test_client_exits_when_server_closes,
]


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else CLIENT

    print(f"Testing {path} against a fake server on an ephemeral port\n")

    for test in TESTS:
        try:
            test(path)
        except (OSError, ValueError) as exc:
            check(test.__name__, False, f"raised {type(exc).__name__}: {exc}")
        time.sleep(0.2)

    passed = sum(RESULTS)
    total = len(RESULTS)
    print(f"\n{passed}/{total} checks passed")

    return 0 if passed == total else 1


if __name__ == "__main__":
    sys.exit(main())