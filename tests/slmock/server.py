"""A scriptable, single-connection-at-a-time SeedLink mock server.

Each test supplies a small `handler(conn, reader, server, conn_index)`
function that reads commands with `reader.read_command()` and writes
raw bytes with `conn.sendall(...)`.  The helpers below implement the
common HELLO/negotiation phases shared by (almost) every scenario so
individual tests only need to script the parts that differ.

SeedLink *commands* are terminated by a bare '\\r' for protocol v4 and
by '\\r\\n' for v3 and for every *response* regardless of protocol
version (see network.c: sl_recvresp() always waits for the full
'\\r\\n' pair).  CommandReader.read_command() copes with both.
"""

import socket
import threading

from . import spec


class ConnectionClosed(Exception):
    pass


class CommandReader:
    def __init__(self, sock, timeout=10.0, strict_protocol=None):
        self.sock = sock
        self.buf = b""
        self.timeout = timeout
        # When set to 3 or 4, every command read is passed through
        # spec.check_command() before being handed to the caller, so a
        # command the client should never send under that protocol
        # version fails the test immediately rather than being silently
        # answered as though it were legal.
        self.strict_protocol = strict_protocol
        # Every command read, as (text, terminator_bytes) -- lets a test
        # assert on the exact wire framing rather than the decoded text.
        self.history = []

    def _fill(self):
        self.sock.settimeout(self.timeout)
        chunk = self.sock.recv(4096)
        if not chunk:
            raise ConnectionClosed()
        self.buf += chunk

    def read_command(self):
        while b"\r" not in self.buf and b"\n" not in self.buf:
            self._fill()

        # A bare '\n' terminator is only legal for v4 (see spec.V4_TERMINATORS);
        # find whichever terminator byte comes first in the buffer.
        idx_cr = self.buf.find(b"\r")
        idx_lf = self.buf.find(b"\n")
        if idx_cr == -1:
            idx = idx_lf
        elif idx_lf == -1:
            idx = idx_cr
        else:
            idx = min(idx_cr, idx_lf)

        cmd = self.buf[:idx]
        if self.buf[idx : idx + 2] == b"\r\n":
            terminator = b"\r\n"
            rest = self.buf[idx + 2 :]
        else:
            terminator = self.buf[idx : idx + 1]
            rest = self.buf[idx + 1 :]
        self.buf = rest

        text = cmd.decode("latin1")
        self.history.append((text, terminator))

        if self.strict_protocol is not None:
            spec.check_command(text, terminator, self.strict_protocol)

        return text

    def try_read_command(self, quiet_seconds):
        """Like read_command(), but returns None instead of blocking if
        no complete command line arrives within `quiet_seconds`. Used to
        find the boundary between a batch of commands a client pipelines
        without waiting for responses and the point where it has
        stopped and is now waiting -- there is no verb to read up to at
        that boundary, only silence."""
        old_timeout = self.timeout
        self.timeout = quiet_seconds
        try:
            return self.read_command()
        except socket.timeout:
            return None
        finally:
            self.timeout = old_timeout


class MockServer:
    def __init__(self, handler, host="127.0.0.1", accept_timeout=0.5, strict_protocol=None):
        self.host = host
        self.handler = handler
        self.accept_timeout = accept_timeout
        self.connection_count = 0
        self.errors = []
        # Passed through to each connection's CommandReader; see its
        # docstring. A handler that negotiates the version dynamically
        # (e.g. deciding v3 vs v4 based on what HELLO advertises) can
        # instead set `reader.strict_protocol` once it knows.
        self.strict_protocol = strict_protocol

        self._listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self._listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self._listener.bind((host, 0))
        self._listener.listen(5)
        self.port = self._listener.getsockname()[1]

        self._stop = False
        self._thread = threading.Thread(target=self._serve, daemon=True)

    def start(self):
        self._thread.start()
        return self

    def _wrap(self, conn):
        """Hook point for subclasses (e.g. TLS) to wrap the accepted socket."""
        return conn

    def _serve(self):
        while not self._stop:
            try:
                self._listener.settimeout(self.accept_timeout)
                conn, _ = self._listener.accept()
            except socket.timeout:
                continue
            except OSError:
                break

            self.connection_count += 1
            conn_index = self.connection_count

            try:
                conn = self._wrap(conn)
            except Exception as e:  # noqa: BLE001
                self.errors.append(e)
                try:
                    conn.close()
                except OSError:
                    pass
                continue

            reader = CommandReader(conn, strict_protocol=self.strict_protocol)

            try:
                self.handler(conn, reader, self, conn_index)
            except ConnectionClosed:
                pass
            except Exception as e:  # noqa: BLE001
                self.errors.append(e)
            finally:
                try:
                    conn.close()
                except OSError:
                    pass

    def stop(self):
        self._stop = True
        try:
            self._listener.close()
        except OSError:
            pass
        self._thread.join(timeout=3)

    def address(self):
        return "127.0.0.1:%d" % self.port

    def __enter__(self):
        self.start()
        return self

    def __exit__(self, *exc):
        self.stop()


def respond(conn, status, extended=None):
    """Send a status response line. If `extended` is given, frame it as
    a v3 "extended reply" (a second '\\r'-terminated segment before the
    final '\\r\\n'), matching the pattern network.c's memchr-based
    parsing expects."""
    if extended is not None:
        conn.sendall((status + "\r" + extended + "\r\n").encode("latin1"))
    else:
        conn.sendall((status + "\r\n").encode("latin1"))


def error_v4(conn, code, description=None):
    """Send a v4 ERROR response in the spec's own shape:
    'ERROR <code> [description]<cr><lf>' (protocol.html, "Error codes").
    Unlike `respond(..., extended=...)`, this never embeds a second
    '\\r' -- v4 error responses are a single line."""
    text = "ERROR " + code + (" " + description if description else "")
    conn.sendall((text + "\r\n").encode("latin1"))


def _check_history(reader, cmd, protocol):
    """Validate the most recently read command (already consumed by the
    caller before handing it to one of the serve_v3_*/serve_v4 helpers
    below) against `protocol`, using its actual terminator bytes."""
    terminator = reader.history[-1][1] if reader.history else b"\r"
    spec.check_command(cmd, terminator, protocol)


def serve_hello(
    reader,
    conn,
    # network.c scans the capability string for repeated "SLPROTO:x.y"
    # tokens individually; each protocol version needs its own token.
    server_id="SeedLink v4.0 (test) :: SLPROTO:3.1 SLPROTO:4.0 CAP CAPABILITIES",
    site="Test Data Center",
):
    cmd = reader.read_command()
    if cmd != "HELLO":
        raise AssertionError("expected HELLO, got %r" % cmd)
    conn.sendall((server_id + "\r\n" + site + "\r\n").encode("latin1"))
    return cmd


def serve_precommands(
    reader,
    conn,
    *,
    accept_capabilities=True,
    accept_useragent=True,
    auth_mode="accept",
    accept_batch=True,
):
    """Consume and answer SLPROTO/CAPABILITIES/USERAGENT/AUTH/BATCH
    commands, returning the first command that isn't one of those (the
    first STATION/SELECT/DATA/FETCH/TIME/END/ENDFETCH command)."""
    while True:
        cmd = reader.read_command()
        verb = cmd.split(" ", 1)[0]

        if verb == "SLPROTO":
            respond(conn, "OK")
        elif verb == "CAPABILITIES":
            respond(conn, "OK" if accept_capabilities else "ERROR")
        elif verb == "USERAGENT":
            respond(conn, "OK" if accept_useragent else "ERROR")
        elif verb == "AUTH":
            if auth_mode == "accept":
                respond(conn, "OK")
            elif auth_mode == "reject":
                error_v4(conn, "AUTH", "invalid credentials")
            else:
                raise ConnectionClosed()
        elif verb == "BATCH":
            respond(conn, "OK" if accept_batch else "ERROR")
        else:
            return cmd


def serve_v3_uni(reader, conn, cmd, *, accept_selectors=True):
    """v3 uni-station negotiation: zero or more SELECT commands (each
    acknowledged) followed by DATA/FETCH/TIME (never acknowledged)."""
    reader.strict_protocol = 3
    _check_history(reader, cmd, 3)
    commands = []

    while cmd.startswith("SELECT"):
        commands.append(cmd)
        respond(conn, "OK" if accept_selectors else "ERROR")
        cmd = reader.read_command()

    commands.append(cmd)
    return commands


def serve_v3_multi(
    reader,
    conn,
    cmd,
    *,
    accept_stations=True,
    accept_selectors=True,
    accept_data=True,
    batch=False,
):
    """v3 multi-station negotiation: per station, STATION + SELECT* + one
    of DATA/FETCH/TIME, each acknowledged unless batch mode is active (in
    which case the client never reads any of these responses), followed
    by a final unacknowledged END."""
    reader.strict_protocol = 3
    _check_history(reader, cmd, 3)
    stations = []

    while cmd.startswith("STATION"):
        stations.append(cmd)
        if not batch:
            respond(conn, "OK" if accept_stations else "ERROR")

        cmd = reader.read_command()

        while cmd.startswith("SELECT"):
            if not batch:
                respond(conn, "OK" if accept_selectors else "ERROR")
            cmd = reader.read_command()

        if not batch:
            respond(conn, "OK" if accept_data else "ERROR")

        cmd = reader.read_command()

    if cmd not in ("END",):
        raise AssertionError("expected END, got %r" % cmd)

    return stations


def serve_v4(
    reader,
    conn,
    cmd,
    *,
    accept=True,
    error_text="ERROR ARGUMENTS rejected",
    defer_responses=False,
):
    """v4 negotiation: the client sends STATION/SELECT/DATA commands for
    every configured station back-to-back, then reads all responses in
    a second pass, so simply answering each as it's read is correct --
    but it does not send END/ENDFETCH until it has read a response to
    every one of those, so there is no verb to read up to at the end of
    the pipelined batch, only silence.

    `defer_responses=True` instead drains that whole pipelined batch
    first (a short quiet period marks where the client stopped sending
    and started waiting), answers all of it at once, and only then
    reads the END/ENDFETCH that follows -- the asynchronous handshaking
    the v4 spec permits (protocol.html, "Handshaking": "Client may send
    asynchronous commands"), proving the client doesn't depend on
    responses trickling back inline with its own sends."""
    reader.strict_protocol = 4
    _check_history(reader, cmd, 4)
    commands = [cmd]

    if defer_responses:
        while True:
            more = reader.try_read_command(0.3)
            if more is None:
                break
            commands.append(more)

        for _ in commands:
            respond(conn, "OK" if accept else error_text)

        end_cmd = reader.read_command()
    else:
        respond(conn, "OK" if accept else error_text)
        cmd = reader.read_command()

        while cmd.startswith("STATION") or cmd.startswith("SELECT") or cmd.startswith("DATA"):
            commands.append(cmd)
            respond(conn, "OK" if accept else error_text)
            cmd = reader.read_command()

        end_cmd = cmd

    if end_cmd not in ("END", "ENDFETCH"):
        raise AssertionError("expected END/ENDFETCH, got %r" % end_cmd)

    return commands, end_cmd
