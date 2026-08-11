#!/usr/bin/env python3
"""Cross-check the public API declared in libslink.h against the two
platform export lists that are supposed to mirror it: libslink.def
(Windows) and libslink.map (the ELF/Darwin symbol-visibility script).

Also cross-checks libslink.def against the actual symbols produced by
the build (via `nm` on ../libslink.a), since a name can be spelled
correctly in the source but wrong in the export list, or vice versa.
"""

import os
import re
import subprocess
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HEADER = os.path.join(ROOT, "libslink.h")
DEF_FILE = os.path.join(ROOT, "libslink.def")
MAP_FILE = os.path.join(ROOT, "libslink.map")
STATIC_LIB = os.path.join(ROOT, "libslink.a")


def public_function_names():
    """Every function declared `extern ... name (...)` in libslink.h.
    This intentionally excludes the static inline sl_gswap2/4/8 helpers,
    which are declared without `extern` and have no external linkage."""
    with open(HEADER, "r", encoding="utf-8") as f:
        text = f.read()

    # Strip comments so a name mentioned only in a doc comment (e.g. the
    # @sa cross-references) is never mistaken for a declaration.
    text = re.sub(r"/\*.*?\*/", " ", text, flags=re.DOTALL)
    text = re.sub(r"//.*", "", text)

    names = []
    for stmt in text.split(";"):
        if "extern" not in stmt:
            continue
        m = re.search(r"\bsl_[A-Za-z0-9_]*\s*\(", stmt)
        if m:
            names.append(m.group(0).split("(")[0].strip())
    return sorted(set(names))


def def_entries():
    with open(DEF_FILE, "r", encoding="utf-8") as f:
        lines = [line.strip() for line in f]
    return sorted(
        line for line in lines if line and not line.startswith(("LIBRARY", "EXPORTS"))
    )


def built_library_symbols():
    """Global text symbols actually present in ../libslink.a, via nm.
    Handles both the Mach-O leading-underscore convention and the
    unprefixed ELF convention."""
    if not os.path.exists(STATIC_LIB):
        raise unittest.SkipTest("../libslink.a is not built; run `make` first")

    out = subprocess.run(
        ["nm", "-g", STATIC_LIB], capture_output=True, text=True, check=False
    ).stdout

    names = set()
    for line in out.splitlines():
        m = re.search(r"\bT\s+_?(sl_[A-Za-z0-9_]+)\s*$", line)
        if m:
            names.add(m.group(1))
    return names


class TestExportConsistency(unittest.TestCase):
    def setUp(self):
        self.public = public_function_names()
        self.defs = def_entries()
        self.real_symbols = built_library_symbols()

    def test_every_public_function_is_in_the_def_file(self):
        missing = sorted(set(self.public) - set(self.defs))
        self.assertEqual(
            missing,
            [],
            "libslink.def is missing exports for public functions declared in "
            "libslink.h: %r" % (missing,),
        )

    def test_every_def_entry_is_a_real_exported_symbol(self):
        bogus = sorted(name for name in self.defs if name not in self.real_symbols)
        self.assertEqual(
            bogus,
            [],
            "libslink.def lists names that are not real exported symbols of the "
            "built library (typos, or -- for sl_gswap2/4/8 -- static inline "
            "functions with no external linkage): %r" % (bogus,),
        )

    def test_map_file_exports_the_sl_prefix(self):
        with open(MAP_FILE, "r", encoding="utf-8") as f:
            content = f.read()
        self.assertIn("sl_*", content, "libslink.map should export the sl_ symbol prefix")


if __name__ == "__main__":
    unittest.main()
