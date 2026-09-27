#!/usr/bin/env python3
"""Offline safety, inventory, and source-build checks for native packages."""

import importlib.util
import ast
import io
import json
from pathlib import Path
import tarfile
import tempfile
import unittest
from unittest.mock import patch
import zipfile

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("prepare_packages", ROOT / "packages/prepare.py")
prepare = importlib.util.module_from_spec(spec)
spec.loader.exec_module(prepare)


def archive(entries, *, link=False):
    output = io.BytesIO()
    with tarfile.open(fileobj=output, mode="w:gz") as result:
        for name, data in entries:
            item = tarfile.TarInfo(name)
            if link:
                item.type = tarfile.SYMTYPE
                item.linkname = "/tmp/outside"
                result.addfile(item)
            else:
                item.size = len(data)
                result.addfile(item, io.BytesIO(data))
    return output.getvalue()


class PackagesTest(unittest.TestCase):
    def test_inventory(self):
        self.assertEqual(set(prepare.PACKAGES), {"glfw", "glad", "sqlite3", "cimgui", "zlib", "libpng", "assimp", "libgit2"})
        lock = json.loads((prepare.ROOT / "upstream-lock.json").read_text())
        self.assertEqual(set(lock), set(prepare.PACKAGES) | {"imgui"})
        for name, entry in lock.items():
            self.assertRegex(entry["sha256"], r"^[0-9a-f]{64}$")
            self.assertTrue(entry["url"].startswith("https://"))
            prepare.contained(prepare.ROOT, entry["destination"])
            if "revision" in entry:
                self.assertRegex(entry["revision"], r"^[0-9a-f]{40}$")
            if name != "imgui":
                self.assertTrue((prepare.ROOT / name / "gti.toml").is_file())

    def test_tar_and_zip(self):
        self.assertEqual(prepare.archive_files(archive([("root/a.h", b"header")])), {"a.h": b"header"})
        data = io.BytesIO()
        with zipfile.ZipFile(data, "w") as result:
            result.writestr("root/a.h", b"header")
        self.assertEqual(prepare.archive_files(data.getvalue(), True), {"a.h": b"header"})

    def test_reject_archive_traversal_special_and_duplicate_files(self):
        cases = [
            archive([("root/../outside", b"bad")]),
            archive([("/absolute/file", b"bad")]),
            archive([("root/.git/config", b"bad")]),
            archive([("root/C:\\outside", b"bad")]),
            archive([("root/a", b"one"), ("root/A", b"two")]),
            archive([("one/a", b"one"), ("two/a", b"two")]),
            archive([("root/link", b"")], link=True),
        ]
        for data in cases:
            with self.subTest(data=data), self.assertRaises(ValueError):
                prepare.archive_files(data)

    def test_large_source_archive_exclusions(self):
        data = archive([("root/code/api.c", b"code"), ("root/test/huge.model", b"model"),
                        ("root/doc/readme.txt", b"docs")])
        self.assertEqual(prepare.archive_files(data, exclude_directories=("test", "doc")),
                         {"code/api.c": b"code"})
        with self.assertRaises(ValueError):
            prepare.archive_files(archive([("root/test/../../outside", b"bad")]),
                                  exclude_directories=("test",))

    def test_consumer_zlib_copy_is_contained_and_idempotent(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / "zlib/vendor/zlib"
            source.mkdir(parents=True)
            (source / "zlib.h").write_text("pinned", encoding="utf-8")
            with patch.object(prepare, "ROOT", root):
                prepare.prepare_consumer_zlib("assimp")
                copied = root / "assimp/vendor/zlib/zlib.h"
                self.assertEqual(copied.read_text(encoding="utf-8"), "pinned")
                copied.write_text("local", encoding="utf-8")
                prepare.prepare_consumer_zlib("assimp")
                self.assertEqual(copied.read_text(encoding="utf-8"), "local")
                with self.assertRaisesRegex(ValueError, "unsafe relative path"):
                    prepare.prepare_consumer_zlib("../outside")

    def test_configured_packages_declare_source_builds(self):
        expected = {
            "glfw": ("vendor/glfw", "GLFW_BUILD_TESTS=OFF"),
            "assimp": ("vendor/assimp", "ASSIMP_BUILD_TESTS=OFF"),
            "libgit2": ("vendor/libgit2", "BUILD_TESTS=OFF"),
        }
        for name, (vendor, option) in expected.items():
            with self.subTest(package=name):
                package = prepare.ROOT / name
                legacy_script = (package / "build.py").read_text(encoding="utf-8")
                ast.parse(legacy_script)
                script = (package / "build.gti").read_text(encoding="utf-8")
                manifest = (package / "gti.toml").read_text(encoding="utf-8")
                self.assertIn("[package.build-script]", manifest)
                self.assertIn('source = "build.gti"', manifest)
                self.assertIn(vendor, manifest)
                self.assertIn(option, script)
                self.assertIn("std::build::context_from_args", script)
                self.assertIn("std::build::command", script)
                self.assertIn("std::build::outputs", script)
                self.assertIn("context.jobs()", script)
                self.assertIn("outputs.publish(context)", script)
        for name in ("assimp", "libgit2"):
            manifest = (prepare.ROOT / name / "gti.toml").read_text(
                encoding="utf-8"
            )
            self.assertIn("vendor/zlib", manifest)
            script = (prepare.ROOT / name / "build.gti").read_text(
                encoding="utf-8"
            )
            self.assertIn('"/vendor/zlib"', script)

    def test_offline_checksum_idempotence_and_modified_sources(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            cache = root / "cache"
            cache.mkdir()
            data = archive([("root/api.h", b"header")])
            entry = {"url": "https://example.invalid/source.tar.gz", "sha256": prepare.digest(data),
                     "destination": "package/vendor/source"}
            with patch.object(prepare, "ROOT", root):
                with self.assertRaisesRegex(ValueError, "offline"):
                    prepare.acquire(entry, cache, True)
                (cache / entry["sha256"]).write_bytes(b"corrupt")
                with self.assertRaisesRegex(ValueError, "checksum mismatch"):
                    prepare.acquire(entry, cache, True)
                (cache / entry["sha256"]).write_bytes(data)
                with self.assertRaisesRegex(ValueError, "size limit"):
                    prepare.acquire({**entry, "max-download-bytes": len(data) - 1}, cache, True)
                prepare.acquire(entry, cache, True)
                prepare.acquire(entry, cache, True)
                header = root / entry["destination"] / "api.h"
                header.write_bytes(b"user edit")
                with self.assertRaisesRegex(ValueError, "refusing to overwrite"):
                    prepare.acquire(entry, cache, True)
                self.assertEqual(header.read_bytes(), b"user edit")

    def test_symlinks_are_not_followed(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "link").symlink_to(root, target_is_directory=True)
            with self.assertRaisesRegex(ValueError, "symlink"):
                prepare.contained(root, "link/destination")


if __name__ == "__main__":
    unittest.main()
