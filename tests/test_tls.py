#!/usr/bin/env python3
"""TLS coverage for libslink's mbedTLS glue in network.c.

Requires the third-party `trustme` package to mint a throwaway CA and
leaf certificate; the whole module is skipped with a clear reason when
it isn't installed, so `make test` still runs everything else on a
stdlib-only machine.

    python3 -m pip install trustme
"""

import os
import ssl
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(__file__))

try:
    import trustme

    HAVE_TRUSTME = True
except ImportError:
    HAVE_TRUSTME = False

from slmock import mseed
from slmock.server import MockServer, serve_hello, serve_precommands, serve_v4
from test_protocol import HARNESS, parse_output


class TLSMockServer(MockServer):
    """A MockServer whose accepted sockets are wrapped in a server-side
    TLS context signed by the given trustme leaf certificate."""

    def __init__(self, handler, leaf_cert, host="127.0.0.1", port=0):
        self._ssl_ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        leaf_cert.configure_cert(self._ssl_ctx)
        super().__init__(handler, host=host)
        if port:
            # Rebind to a caller-specified port (used only for the
            # port-18500-implies-TLS test).
            self._listener.close()
            import socket

            self._listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            self._listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            self._listener.bind((host, port))
            self._listener.listen(5)
            self.port = port

    def _wrap(self, conn):
        return self._ssl_ctx.wrap_socket(conn, server_side=True)


@unittest.skipUnless(HAVE_TRUSTME, "trustme not installed (pip install trustme); TLS tests skipped")
class TestTLS(unittest.TestCase):
    def setUp(self):
        self.ca = trustme.CA()
        self.leaf = self.ca.issue_cert("localhost", "127.0.0.1")
        self.tmpdir = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmpdir.cleanup)
        self.ca_file = os.path.join(self.tmpdir.name, "ca.pem")
        self.ca.cert_pem.write_to_path(self.ca_file)

    def run_tls_scenario(self, handler, args, leaf=None, timeout=15, expect_timeout=False):
        server = TLSMockServer(handler, leaf or self.leaf).start()
        self.addCleanup(server.stop)

        full_args = [HARNESS, "--address", server.address(), "--tls"] + args

        try:
            proc = subprocess.run(full_args, capture_output=True, text=True, timeout=timeout)
        except subprocess.TimeoutExpired as e:
            if expect_timeout:
                stdout = e.stdout or ""
                if isinstance(stdout, bytes):
                    stdout = stdout.decode("utf-8", "replace")
                return parse_output(stdout), server
            self.fail("slharness did not exit within %s seconds; stdout so far:\n%s" % (e.timeout, e.stdout))

        if expect_timeout:
            self.fail("expected slharness to hang retrying, but it exited with %r" % (proc.returncode,))

        if server.errors:
            self.fail("mock server handler raised: %r" % (server.errors,))

        events = parse_output(proc.stdout)
        events["returncode"] = proc.returncode
        return events, server

    def test_v4_negotiation_over_tls(self):
        def handler(conn, reader, server, idx):
            serve_hello(reader, conn)
            cmd = serve_precommands(reader, conn)
            serve_v4(reader, conn, cmd)
            record = mseed.build_ms3(sid="FDSN:XX_TEST_00_B_H_Z", samplerate=100.0)
            conn.sendall(mseed.frame_v4_data(1, "XX_TEST", record))

        events, _ = self.run_tls_scenario(
            handler,
            [
                "--v4",
                "--ca-file",
                self.ca_file,
                "--station",
                "XX_TEST:BHZ",
                "--max-packets",
                "1",
                "--timeout-seconds",
                "8",
            ],
        )

        self.assertEqual(len(events["packets"]), 1, events)
        self.assertTrue(
            any("TLS connection established" in line for line in events["log"]), events["log"]
        )

    def test_ca_cert_path_directory_form(self):
        def handler(conn, reader, server, idx):
            serve_hello(reader, conn)
            cmd = serve_precommands(reader, conn)
            serve_v4(reader, conn, cmd)
            record = mseed.build_ms3(sid="FDSN:XX_TEST_00_B_H_Z", samplerate=100.0)
            conn.sendall(mseed.frame_v4_data(1, "XX_TEST", record))

        # LIBSLINK_CA_CERT_PATH points mbedtls_x509_crt_parse_path() at a
        # directory of PEM files rather than a single file.
        capath = os.path.join(self.tmpdir.name, "ca_dir")
        os.mkdir(capath)
        self.ca.cert_pem.write_to_path(os.path.join(capath, "ca.pem"))

        events, _ = self.run_tls_scenario(
            handler,
            [
                "--v4",
                "--ca-path",
                capath,
                "--station",
                "XX_TEST:BHZ",
                "--max-packets",
                "1",
                "--timeout-seconds",
                "8",
            ],
        )

        self.assertEqual(len(events["packets"]), 1, events)

    def test_untrusted_certificate_is_rejected(self):
        other_ca = trustme.CA()
        untrusted_leaf = other_ca.issue_cert("localhost", "127.0.0.1")

        def handler(conn, reader, server, idx):
            # Never reached: the handshake itself must fail.
            pass

        events, _ = self.run_tls_scenario(
            handler,
            [
                "--v4",
                "--ca-file",
                self.ca_file,  # trusted CA does NOT match the server's cert
                "--station",
                "XX_TEST:BHZ",
                "--reconnectdelay",
                "3",
                "--timeout-seconds",
                "4",
            ],
            leaf=untrusted_leaf,
            timeout=12,
            expect_timeout=True,
        )

        self.assertTrue(
            any("certificate verification failed" in line.lower() for line in events["log"]),
            events["log"],
        )
        self.assertEqual(events["packets"], [])

    def test_unsupported_tls_cert_env_var_name_is_not_honored(self):
        """LIBSLINK_CA_CERT_FILE/_PATH are the only supported CA env var
        names (see load_ca_certs()); LIBSLINK_TLS_CERT_FILE is not read, so
        it must not be treated as a substitute."""

        def handler(conn, reader, server, idx):
            # Never reached: no CA is loaded, so the handshake must fail.
            pass

        env_backup = dict(os.environ)
        os.environ.pop("LIBSLINK_CA_CERT_FILE", None)
        os.environ["LIBSLINK_TLS_CERT_FILE"] = self.ca_file
        self.addCleanup(lambda: os.environ.clear() or os.environ.update(env_backup))

        events, _ = self.run_tls_scenario(
            handler,
            [
                "--v4",
                "--station",
                "XX_TEST:BHZ",
                "--reconnectdelay",
                "3",
                "--timeout-seconds",
                "4",
            ],
            timeout=12,
            expect_timeout=True,
        )

        self.assertTrue(
            any("certificate verification failed" in line.lower() for line in events["log"]),
            events["log"],
        )
        self.assertEqual(events["packets"], [])


if __name__ == "__main__":
    unittest.main()
