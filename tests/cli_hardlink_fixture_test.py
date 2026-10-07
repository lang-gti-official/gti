#!/usr/bin/env python3
"""Keep the executable-overwrite probe inside a relocatable scratch prefix."""

import pathlib
import tempfile
import unittest
from unittest.mock import patch

from cli_smoke_test import producer_report_alias_path


class ProducerAliasTests(unittest.TestCase):
    def setUp(self):
        self.scratch = tempfile.TemporaryDirectory(prefix="gti-alias-fixture-")
        self.addCleanup(self.scratch.cleanup)
        self.root = pathlib.Path(self.scratch.name)
        self.original = self.root / "installed"
        self.binary = self.original / "bin" / "gti"
        self.binary.parent.mkdir(parents=True)
        self.binary.write_bytes(b"preserved compiler")
        for directory in ("lib", "lib64", "share", "include"):
            location = self.original / directory
            location.mkdir()
            (location / "context").write_text(directory, encoding="utf-8")
        (self.binary.parent / "runtime.DLL").write_bytes(b"adjacent runtime")
        self.fixture = self.root / "fixture"
        self.fixture.mkdir()

    def check_layout(self, alias):
        self.assertEqual(alias.parent.name, "bin")
        self.assertFalse(alias.exists())
        for directory in ("lib", "lib64", "share", "include"):
            self.assertEqual(
                (alias.parent.parent / directory / "context").read_text(), directory
            )
        self.assertEqual(
            (alias.parent / "runtime.DLL").read_bytes(), b"adjacent runtime"
        )
        self.assertEqual(self.binary.read_bytes(), b"preserved compiler")
        self.assertEqual(
            sorted(path.name for path in self.original.iterdir()),
            ["bin", "include", "lib", "lib64", "share"],
        )

    def test_prefix_context_and_same_inode_overwrite_target(self):
        alias = producer_report_alias_path(self.binary, self.fixture)
        self.check_layout(alias)
        alias.hardlink_to(self.binary)
        self.assertTrue(alias.samefile(self.binary))
        self.assertEqual(alias.read_bytes(), b"preserved compiler")

    def test_unprivileged_copy_fallback_preserves_context(self):
        with patch.object(pathlib.Path, "symlink_to", side_effect=OSError("denied")):
            alias = producer_report_alias_path(self.binary, self.fixture)
        self.check_layout(alias)
        self.assertFalse((alias.parent.parent / "lib").is_symlink())
        self.assertFalse((alias.parent / "runtime.DLL").is_symlink())
        alias.hardlink_to(self.binary)
        self.assertTrue(alias.samefile(self.binary))

    def test_build_tree_without_installed_context(self):
        build = self.root / "uninstalled" / "build"
        build.mkdir(parents=True)
        compiler = build / "gti"
        compiler.write_bytes(b"build-tree compiler")
        alias = producer_report_alias_path(compiler, self.fixture)
        self.assertEqual(list(alias.parent.parent.iterdir()), [alias.parent])
        alias.hardlink_to(compiler)
        self.assertTrue(alias.samefile(compiler))


if __name__ == "__main__":
    unittest.main()
