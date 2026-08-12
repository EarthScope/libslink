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
out short keepalive/reconnect timers.

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
  requirement it enforces.
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

