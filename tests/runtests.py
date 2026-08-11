#!/usr/bin/env python3
"""Test runner for libslink: runs every compiled test_*.c binary (TAP
output, exit code decides pass/fail) and every test_*.py module (via
the stdlib unittest runner), and reports one combined summary.

Usage:
    python3 runtests.py [-v] [-k PATTERN]

-k PATTERN filters C binaries by substring match on their name, and is
forwarded as unittest's own -k to each Python module.
"""

import argparse
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))

# Kept as an explicit list rather than discovered by globbing so that a
# test_*.c source with no corresponding built binary (e.g. a fresh clone
# before `make`) is reported as "not built" instead of silently skipped.
C_BINARIES = [
    "test_genutils",
    "test_globmatch",
    "test_payload",
    "test_slcd",
    "test_streams",
    "test_statefile",
    "test_logging",
    "test_internals",
    "test_netprims",
]

PYTHON_MODULES = [
    "test_protocol",
    "test_spec_v3",
    "test_spec_v4",
    "test_tls",
    "test_exports",
]


def run_c_binary(name, verbose):
    path = os.path.join(HERE, name)

    if not os.path.exists(path):
        print("SKIP %s (not built -- run `make` in tests/)" % name)
        return None

    # A C test binary's stderr can legitimately contain raw bytes (e.g. a
    # test deliberately feeding non-ASCII/binary data through the
    # library's own %s-based error logging); decode leniently rather
    # than let a runner-level UnicodeDecodeError mask the actual result.
    proc = subprocess.run([path], capture_output=True, cwd=HERE)
    proc_stdout = proc.stdout.decode("utf-8", "replace")
    proc_stderr = proc.stderr.decode("utf-8", "replace")
    ok = proc.returncode == 0

    print("%s %s" % ("PASS" if ok else "FAIL", name))
    if verbose or not ok:
        sys.stdout.write(proc_stdout)
        sys.stderr.write(proc_stderr)

    return ok


def run_python_module(name, verbose, pattern):
    cmd = [sys.executable, "-m", "unittest", name]
    if verbose:
        cmd.append("-v")
    if pattern:
        cmd += ["-k", pattern]

    proc = subprocess.run(cmd, cwd=HERE)
    ok = proc.returncode == 0

    print("%s %s" % ("PASS" if ok else "FAIL", name))
    return ok


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("-v", "--verbose", action="store_true")
    ap.add_argument("-k", dest="pattern", default=None, help="filter by substring/unittest -k pattern")
    args = ap.parse_args()

    results = {}

    for name in C_BINARIES:
        if args.pattern and args.pattern.lower() not in name.lower():
            continue
        r = run_c_binary(name, args.verbose)
        if r is not None:
            results[name] = r

    for name in PYTHON_MODULES:
        results[name] = run_python_module(name, args.verbose, args.pattern)

    print()
    passed = sum(1 for ok in results.values() if ok)
    print("%d/%d test groups passed" % (passed, len(results)))

    failed = sorted(n for n, ok in results.items() if not ok)
    if failed:
        print("FAILED: %s" % ", ".join(failed))
        print()
        print("See tests/README.md for the current baseline of expected")
        print("known-issue failures before treating this as a regression.")

    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
