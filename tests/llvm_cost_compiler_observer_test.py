#!/usr/bin/env python3
"""Actual-input retention, failure propagation and tamper controls."""

import hashlib
import json
from pathlib import Path
import sys
import tempfile
import unittest

from llvm_cost_compiler_observer import observe


class ObserverControls(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(prefix="gti-compiler-observer-")
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        self.compiler = self.root / "native-compiler"
        self.compiler.write_text(f"#!{sys.executable}\n" + '''import pathlib,sys
args=sys.argv[1:]
if '--fail' in args: raise SystemExit(7)
out=pathlib.Path(args[args.index('-o')+1]);out.parent.mkdir(exist_ok=True)
source=next(pathlib.Path(x) for x in args if x.endswith('.cpp'))
if '--mutate-input' in args: source.write_bytes(b'changed')
out.write_bytes(b'native executable')
out.with_suffix('.bc').write_bytes(b'actual bitcode output')
''')
        self.compiler.chmod(0o755)
        self.source = self.root / "source with spaces.cpp"
        self.source.write_bytes(b"int main() { return 0; }\n")
        self.output = self.root / "staging/program"
        self.output.parent.mkdir()
        self.capture = self.root / "captures"

    def run_observer(self, extra=()):
        return observe(self.compiler, self.capture,
                       [str(self.source), *extra, "-o", str(self.output)])

    def record(self):
        return json.loads((self.capture / "program/record.json").read_text())

    def test_actual_input_bitcode_and_output_are_retained(self):
        self.assertEqual(self.run_observer(), 0)
        row = self.record()
        self.assertEqual(row["argv"][1], str(self.source))
        original = row["inputs"][0]
        self.assertEqual(original["sha256"], hashlib.sha256(self.source.read_bytes()).hexdigest())
        self.assertEqual((self.capture / "program" / original["retained"]).read_bytes(), self.source.read_bytes())
        self.assertEqual({Path(item["original"]).suffix for item in row["outputs"]}, {"", ".bc"})
        self.assertEqual(row["changed_inputs"], [])

    def test_symlink_keeps_the_invoked_driver_mode_name(self):
        invoked = self.root / "clang++"
        invoked.symlink_to(self.compiler)
        self.assertEqual(observe(invoked, self.capture,
                                 [str(self.source), "-o", str(self.output)]), 0)
        self.assertEqual(self.record()["argv"][0], str(invoked))
        self.assertEqual(self.record()["compiler"]["implementation_path"], str(self.compiler.resolve()))

    def test_native_failure_remains_failure_with_evidence(self):
        self.assertEqual(self.run_observer(["--fail"]), 7)
        self.assertEqual(self.record()["exit"], 7)

    def test_changed_input_cannot_keep_a_successful_binding(self):
        self.assertEqual(self.run_observer(["--mutate-input"]), 125)
        self.assertEqual(self.record()["changed_inputs"], [str(self.source.resolve())])

    def test_duplicate_output_cannot_overwrite_prior_evidence(self):
        self.assertEqual(self.run_observer(), 0)
        before = (self.capture / "program/record.json").read_bytes()
        with self.assertRaises(FileExistsError):
            self.run_observer()
        self.assertEqual((self.capture / "program/record.json").read_bytes(), before)


if __name__ == "__main__":
    unittest.main()
