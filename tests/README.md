# libslink test suite

## Running

```sh
make                      # from the repo root: build libslink.a
make test                 # build and run the whole suite
make test ARGS='-v'       # verbose
make test ARGS='-k v4'    # only tests whose name contains "v4"
make test ARGS='-k spec'  # only the spec-conformance modules (test_spec_v3/v4)

cd tests && ./test_genutils     # run one C binary directly, TAP output
cd tests && python3 -m unittest test_protocol -v   # run one Python module directly
```

Runtime is under a minute; `test_protocol.py`, `test_spec_v3.py`,
`test_spec_v4.py`, and `test_tls.py` dominate since a few scenarios wait
out short keepalive/reconnect timers, or deliberately hit a subprocess
timeout to demonstrate a hang (see "Spec-conformance deviations" below).

### Sanitizer run

```sh
make clean
CFLAGS="-fsanitize=address,undefined -g -O1" make
cd tests && make clean && CFLAGS="-fsanitize=address,undefined -g -O1" make test
```

### TLS tests

```sh
python3 -m pip install trustme    # optional; test_tls.py skips cleanly without it
make test ARGS='-k tls'
```

## Layout

- `slt.h` — header-only TAP assertion framework used by every `test_*.c` binary.
- `fixtures.c`/`.h` — synthetic miniSEED 2/3 record builders and temp-file helpers.
- `test_genutils.c`, `test_globmatch.c`, `test_payload.c`, `test_slcd.c`,
  `test_streams.c`, `test_statefile.c`, `test_logging.c` — unit tests for
  the network-free public API.
- `test_internals.c` — `#include "../slutils.c"` to reach the file-static
  `detect()`, `receive_header()`, `update_stream()` helpers directly.
- `test_netprims.c` — `sl_connect()`/`sl_senddata()`/etc. against a bare
  hand-rolled loopback listener, no protocol negotiation.
- `slharness.c` — a small deterministic CLI client (see its header
  comment for the flags and output format) used only by the two files
  below.
- `slmock/` — a scriptable SeedLink mock server (`server.py`), synthetic
  record/wire-framing builders (`mseed.py`), and the published protocol
  specs' own vocabulary (`spec.py` — error codes, reserved format pairs,
  capability tokens, legal command verbs per version, and
  `check_command()`, which `server.py`'s `serve_v3_*()`/`serve_v4()`
  helpers run every command through once the version is known).
- `test_protocol.py` — drives `slharness` against `slmock` for v3/v4
  negotiation, reconnects, keepalives, INFO, errors, and more. Organized
  around the current implementation's own code paths.
- `test_spec_v3.py` / `test_spec_v4.py` — organized instead around the
  published specs (see each file's module docstring for the URL): one
  test class per spec section, each test's docstring citing the
  requirement it enforces. Where libslink diverges, the test asserts the
  *spec's* behavior and fails on purpose — see "Spec-conformance
  deviations" below.
- `test_tls.py` — the same style as `test_protocol.py`, over TLS; skipped
  if `trustme` isn't installed.
- `test_exports.py` — cross-checks `libslink.h`'s public API against
  `libslink.def` and `libslink.map`.
- `runtests.py` — the entry point `make test` invokes; runs every C
  binary and every Python module and reports one combined summary.

## Adding a test

- New pure-function or SLCD-state coverage: add a test function to the
  relevant `test_*.c` file (or a new file, then add it to `C_BINARIES` in
  both `Makefile` and `runtests.py`) using the `SLT_*` macros in `slt.h`.
- New protocol scenario exercising the current implementation: add a test
  method to `test_protocol.py`. New behavior a published spec actually
  requires: add it to `test_spec_v3.py`/`test_spec_v4.py` instead, in the
  class for the relevant spec section (or a new class, named after the
  section, if none fits), with a docstring citing the requirement.
  Either way, write a `handler(conn, reader, server, idx)` closure using
  the `serve_*()` helpers in `slmock/server.py` for the negotiation
  phase, then send packets with `slmock/mseed.py`'s builders. Read
  `slharness.c`'s header comment for its CLI flags and output lines.
- `--station NET_STA:SELECTORS` always enables **multi**-station mode
  (it calls `sl_add_stream()`, which sets `multistation=1` regardless of
  how many stations are added). For a true v3 **uni**-station scenario
  (no `STATION` command at all), use `--allstation SELECTORS` instead
  (`sl_set_allstation_params()`).
- A scenario the client never returns from at all (e.g. a header field
  the read loop can't finish processing) can't be bounded by
  `--timeout-seconds` — that check only runs *between* `sl_collect()`
  calls, not inside a blocked one. Pass `subprocess_timeout=` to
  `run_scenario()` instead; a real hang then surfaces as a normal
  `self.fail()` from the resulting `subprocess.TimeoutExpired`, which is
  exactly the failure such a test wants to demonstrate.

## Known-issue baseline

Two tables make up the current baseline — a *new* failure beyond both of
them means a regression, not a pre-existing known issue. Each documents
tests that are written to assert **correct** behavior and so show up as
`not ok` / `FAIL` today, flipping to passing once the underlying issue is
fixed.

### Spec-conformance deviations

Found while writing `test_spec_v3.py`/`test_spec_v4.py` against the
published specs (see each file's module docstring for the URL) rather
than against the implementation's own behavior:

| Spec section | Deviation | Test |
|---|---|---|
| v3 "SeedLink packet structure" (six-digit hex sequence field) | `negotiate_uni_v3()`/`negotiate_multi_v3()` (`network.c`) format a resumption sequence with `"%0" PRIX64` — the `0` flag has no effect without an explicit width, so a sequence one past the 24-bit boundary is sent as 7+ hex digits, not wrapped into six | `test_spec_v3.TestCommandSyntax.test_data_sequence_number_should_stay_within_six_hex_digits` |

**Also found while building this suite, sanitizer-only (like Finding 5
below):** `sl_add_stream()` (`slutils.c:1622`) copies a station ID into a
plain `malloc()`'d (not zeroed) `SLstream` with
`strncpy(newstream->stationid, stationid, sizeof(...) - 1)`. For a
station ID of exactly 21 bytes (one less than `SL_MAX_STATIONID`),
`strncpy`'s source has no NUL within the copied range, so the buffer's
last byte is left as whatever the allocator happened to leave there
instead of being zeroed or explicitly terminated. A plain build's first
allocation of a given size is typically zeroed fresh memory from the OS,
which masks this reliably; under `-fsanitize=address` the station ID
intermittently fails to match anything and the connection is dropped.
`test_spec_v4.TestPacketHeader.test_station_id_of_21_bytes_is_accepted`
passes in a plain build; run it under ASan to see the failure.

### Implementation-behavior findings (`fable-review.md`)

`fable-review.md` lists 12 findings against this codebase, plus an
addendum finding (13) from protocol-spec conformance testing. Findings
1, 3, 4, 5, 6, 7, and 13 are fixed (`ChangeLog` `2026.222`) and are
covered by regression tests (`TestKeepaliveAndInfoRegression` and
`TestAuthValueNullRegression`
in `test_protocol.py`, `TestTLS.test_unsupported_tls_cert_env_var_name_is_not_honored`
in `test_tls.py`, `test_slcd.test_auth_envvars`, which asserts that
`auth_data` is released both by a subsequent authentication call and by
`sl_freeslcd()`, `test_slcd.test_serveraddress_host_boundary`, which
covers a 300-character host, `test_netprims.test_senddata_partial_write`,
which shrinks both ends' socket buffers and pushes a buffer larger than
either to force a short `send()` (leak-freedom itself is only observable
under a leak-detecting tool such as `leaks` or `-fsanitize=address`),
and `test_spec_v4.TestErrorCodes.test_connection_closed_after_partial_negotiation_response_should_not_crash`,
which covers finding 13: `negotiate_v4()` (`network.c`) sent every
`STATION`/`SELECT`/`DATA` command for a stream up front, then read one
response per command in a second pass, always passing `command=NULL` to
`sl_recvresp()` for that second-pass read; `sl_recvresp()` unconditionally
called `strcspn(command, "\r\n")` in its own error-logging path when a
read failed, so a server that answered the first queued command and then
closed the connection before answering the rest — a normal, spec-legal
thing to do — dereferenced the `NULL` `command` argument and segfaulted
the client). Findings that are reachable and deterministic
through this suite are written as ordinary tests asserting the
**correct** behavior, so they show up as `not ok` / `FAIL` today and
will flip to passing once each is fixed. This is the current baseline
for this table:

| # | Finding | Test |
|---|---|---|
| 8 | B1000 record-length shift UB (`slutils.c`) | `test_internals.test_detect_ms2_b1000_reclen_overflow` |
| 9 | `config.c`'s `%199c` captures trailing whitespace | `test_streams.test_streamlist_file_trailing_whitespace` |
| — | `libslink.def` export list is out of sync with `libslink.h` (typos, two missing entries, three static-inline names wrongly listed) | `test_exports.py`, both test methods |

**Found while building this suite, not in `fable-review.md`:**
`sl_set_clientname()` frees the existing `clientversion` string but, when
called again with a `NULL` version, never resets the pointer to `NULL`.
A later `sl_freeslcd()` then double-frees it — deterministically aborts
on this platform's allocator. Covered by
`test_slcd.test_clientname_version_dangling_pointer`.

**Documented but not exercised as failing tests** (need instrumentation
or infrastructure this suite doesn't build):

- **Finding 2** (`memchr` length computed with the wrong sign, five sites
  in `network.c`, e.g. `sl_senddata()`'s reply buffer being scanned past
  its intended window). The reply buffer is `memset` to zero right
  before the read, so in a plain build the over-scan almost always lands
  on padding zero bytes and behaves identically to the correct
  computation — it only reads *out of the array's bounds* (and is then
  guaranteed to be caught) when the response places its first `\r` far
  enough into the buffer. This suite does not attempt that construction;
  it would need to be verified under `-fsanitize=address`.
- **Finding 11** (`sl_disconnect()` calls the process-global
  `mbedtls_psa_crypto_free()`, so disconnecting one TLS `SLCD` can break
  another). Not covered: needs a dedicated two-connection C program
  (the `slharness`/mock-server model here only drives one connection
  per process).
- **Finding 10** (duplicated `NULL` check in `globmatch.c`) is dead code
  with no observable behavior; intentionally not tested.

**Rejected:**

- **Finding 16** (an `ERROR` response to `SLPROTO 4.0` should fall back to a
  v3 handshake on the same connection). `sayhello_int()` (`network.c:1184`)
  only sends `SLPROTO 4.0` when the server's own HELLO capabilities
  advertised `SLPROTO:4.x`, or the caller explicitly forced v4 via
  `sl_set_protocol()`. An `ERROR` reply is therefore the server
  contradicting capabilities it just advertised (or the caller's explicit
  request), not a server that merely prefers v3 — treating that as fatal
  and logging it, rather than silently downgrading past it, is the correct
  behavior. Pinned by
  `test_spec_v4.TestErrorCodes.test_error_to_slproto_from_a_v4_advertising_server_is_fatal`.
