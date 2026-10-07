#!/usr/bin/env python3
"""Portable dependency discovery, attribution, relocation, and failure gates."""

import importlib.util
import pathlib
import sys
import tempfile
import unittest
from unittest import mock


spec = importlib.util.spec_from_file_location("bundler", sys.argv.pop(1))
bundler = importlib.util.module_from_spec(spec)
spec.loader.exec_module(bundler)


class RuntimeBundleTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="gti-runtime-bundle-")
        self.addCleanup(self.temporary.cleanup)
        self.root = pathlib.Path(self.temporary.name).resolve()
        self.stage = self.root / "stage"
        self.cli = self.file("stage/bin/gti")
        self.lsp = self.file("stage/bin/gti_lsp")
        self.clang = self.file("Cellar/llvm/22.1.8/lib/libclang.dylib")
        self.llvm = self.file("Cellar/llvm/22.1.8/lib/libLLVM.dylib")
        self.z3 = self.file("Cellar/z3/4.16.0/lib/libz3.4.16.dylib")
        self.file("Cellar/llvm/22.1.8/LICENSE.TXT")
        self.file("Cellar/z3/4.16.0/LICENSE.txt")
        self.clang_id = str(self.root / "opt/llvm/lib/libclang.dylib")
        self.z3_id = str(self.root / "opt/z3/lib/libz3.4.16.dylib")
        self.identities = {self.clang: self.clang_id, self.llvm: "@rpath/libLLVM.dylib",
                           self.z3: self.z3_id}
        self.edges = {self.cli: [self.clang_id, self.z3_id], self.lsp: [self.clang_id],
                      self.clang: ["@rpath/libLLVM.dylib"], self.llvm: [self.z3_id],
                      self.z3: ["/usr/lib/libSystem.B.dylib"]}
        self.addCleanup(mock.patch.stopall)
        mock.patch.object(bundler, "install_id", side_effect=lambda path:
                          self.identities.get(path)).start()
        mock.patch.object(bundler, "dependencies", side_effect=lambda path:
                          self.edges.get(path, [])).start()
        mock.patch.object(bundler, "rpaths", return_value=[]).start()

    def file(self, relative):
        path = self.root / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text("fixture")
        return path

    def test_selected_library_wins_over_upgraded_opt_alias(self):
        newer = self.file("Cellar/llvm/23.1.2/lib/libclang.dylib")
        alias = self.root / "opt/llvm"
        alias.parent.mkdir()
        alias.symlink_to(newer.parent.parent)
        stage, roots, libraries, edges, licenses = bundler.plan_bundle(self.stage, [self.clang])
        self.assertEqual(roots, [self.cli, self.lsp])
        self.assertIn(self.clang, libraries)
        self.assertNotIn(newer, libraries)
        self.assertEqual(edges[self.cli][0][1], self.clang)
        self.assertEqual(len(libraries), 3)
        self.assertEqual(len(licenses), 3)

    def test_never_aliases_a_missing_abi_to_another_version(self):
        self.z3.unlink()
        self.file("Cellar/z3/5.1.0/lib/libz3.5.1.dylib")
        with self.assertRaisesRegex(ValueError, "cannot resolve"):
            bundler.plan_bundle(self.stage, [self.clang])
        self.assertFalse((self.stage / "lib").exists())

    def test_ambiguous_retained_dependency_is_rejected(self):
        self.file("Cellar/z3/4.16.1/lib/libz3.4.16.dylib")
        with self.assertRaisesRegex(ValueError, "multiple retained"):
            bundler.plan_bundle(self.stage, [self.clang])

    def test_inherited_loader_rpath_uses_its_declaring_owner(self):
        library = self.file("stage/bin/private/libExtra.dylib")
        self.edges[self.llvm].append("@rpath/libExtra.dylib")
        with mock.patch.object(bundler, "rpaths", side_effect=lambda owner:
                               ["@loader_path/private"] if owner == self.cli else []):
            # Both roots must provide the same closure; exercise the CLI only.
            self.lsp.unlink()
            _, _, libraries, _, _ = bundler.plan_bundle(self.stage, [self.clang])
            self.assertIn(library, libraries)

    def test_missing_license_fails_before_staging(self):
        (self.root / "Cellar/z3/4.16.0/LICENSE.txt").unlink()
        with self.assertRaisesRegex(ValueError, "no discoverable license"):
            bundler.plan_bundle(self.stage, [self.clang])
        self.assertFalse((self.stage / "lib").exists())

    def test_symlinked_staging_parent_cannot_overwrite_outside(self):
        outside = self.root / "outside"
        outside.mkdir()
        (self.stage / "lib").symlink_to(outside)
        with self.assertRaisesRegex(ValueError, "escapes"):
            bundler.plan_bundle(self.stage, [self.clang])
        self.assertEqual(list(outside.iterdir()), [])

    def test_rewrites_closure_signs_copies_and_records_manifest(self):
        original = self.clang.read_bytes()
        calls = []
        def command(arguments):
            calls.append(arguments)
            if arguments[0] == "install_name_tool":
                owner = pathlib.Path(arguments[-1])
                replacements = {}
                for index, argument in enumerate(arguments):
                    if argument == "-change":
                        replacements[arguments[index + 1]] = arguments[index + 2]
                source = next((path for path in self.edges if path.name == owner.name), owner)
                self.edges[owner] = [replacements.get(name, name) for name in self.edges[source]]
            return ""
        with mock.patch.object(bundler, "command", side_effect=command):
            self.assertEqual(bundler.bundle(self.stage, [self.clang]), 3)
        self.assertEqual(self.clang.read_bytes(), original)
        self.assertEqual(len([call for call in calls if call[0] == "codesign"]), 5)
        self.assertIn("host-runtime/libLLVM.dylib",
                      (self.stage / "share/gti/host-runtime.json").read_text())
        self.assertTrue((self.stage / "share/licenses/gti/host-runtime/libLLVM.dylib/LICENSE.TXT").is_file())
        for name in self.edges[self.cli]:
            self.assertTrue(name.startswith("@loader_path/"))


if __name__ == "__main__":
    unittest.main()
