#!/usr/bin/env python3
"""Failure controls for runtime-cost producer and material attribution."""

import copy
import hashlib
from pathlib import Path
import tempfile
import unittest

from llvm_runtime_cost_test import validate_build_report


class BuildReportControls(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(prefix="gti-cost-provenance-")
        self.addCleanup(self.directory.cleanup)
        root = Path(self.directory.name)

        def material(name):
            path = root / name
            path.write_bytes(name.encode())
            return {"canonical_path": str(path), "bytes": path.stat().st_size,
                    "sha256": hashlib.sha256(path.read_bytes()).hexdigest()}

        self.producer = hashlib.sha256(b"producer").hexdigest()
        self.artifact = material("program")
        self.output = Path(self.artifact["canonical_path"])
        self.native = material("native-compiler")
        self.report = {
            "schema": 2,
            "producer": {"executable": {"sha256": self.producer},
                         "compiler_build": {"state": "clean", "commit": "1" * 40}},
            "artifact": self.artifact,
            "sources": {"units": [{"material": material("source.gti")}],
                        "materials_sha256": "2" * 64},
            "native": {"compiler_executable": self.native,
                       "argv": [self.native["canonical_path"], "program.o"],
                       "runtime": {"archive": material("runtime.a"),
                                   "headers": [{"material": material("runtime.h")}],
                                   "materials_sha256": "3" * 64}}}

    def validate(self, report=None):
        return validate_build_report(report or self.report, self.producer, self.output)

    def test_clean_producer_and_materials_bind(self):
        facts = self.validate()
        self.assertEqual(facts["gti_source_head"], "1" * 40)
        self.assertEqual(facts["native_compiler"], self.native)

    def test_other_compiler_executable_rejects(self):
        report = copy.deepcopy(self.report)
        report["producer"]["executable"]["sha256"] = "0" * 64
        with self.assertRaisesRegex(ValueError, "another GTI executable"):
            self.validate(report)

    def test_same_size_native_material_tampering_rejects(self):
        path = Path(self.native["canonical_path"])
        path.write_bytes(b"X" * path.stat().st_size)
        with self.assertRaisesRegex(ValueError, "material changed"):
            self.validate()

    def test_changed_artifact_rejects(self):
        self.output.write_bytes(b"changed")
        with self.assertRaisesRegex(ValueError, "material changed"):
            self.validate()

    def test_other_artifact_path_rejects(self):
        report = copy.deepcopy(self.report)
        report["artifact"]["canonical_path"] += ".other"
        with self.assertRaisesRegex(ValueError, "another output artifact"):
            self.validate(report)

    def test_dirty_and_unavailable_never_claim_exact_clean_source(self):
        for state in ("dirty", "unavailable"):
            with self.subTest(state=state):
                report = copy.deepcopy(self.report)
                report["producer"]["compiler_build"]["state"] = state
                self.assertIsNone(self.validate(report)["gti_source_head"])

    def test_missing_clean_commit_rejects(self):
        report = copy.deepcopy(self.report)
        report["producer"]["compiler_build"]["commit"] = None
        with self.assertRaisesRegex(ValueError, "no exact source commit"):
            self.validate(report)

    def test_missing_native_invocation_rejects(self):
        report = copy.deepcopy(self.report)
        report["native"]["argv"] = []
        with self.assertRaisesRegex(ValueError, "no actual native invocation"):
            self.validate(report)


if __name__ == "__main__":
    unittest.main()
