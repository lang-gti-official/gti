#!/usr/bin/env python3
"""A release cut turns pending fragments into one CHANGELOG section and the next VERSION."""

from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
import cut_release as module


CHANGELOG = "# Changelog\n\nPreamble.\n\n## 0.4.2 - Earlier\n\n- Earlier change.\n"


class CutRelease(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="gti-cut-release-")
        self.root = Path(self.temp.name)
        (self.root / "VERSION").write_text("0.4.2\n")
        (self.root / "CHANGELOG.md").write_text(CHANGELOG)
        self.changes = self.root / "changes"
        self.changes.mkdir()
        (self.changes / "README.md").write_text("How to write fragments.\n")

    def tearDown(self):
        self.temp.cleanup()

    def fragment(self, name, text):
        (self.changes / name).write_text(text)

    def test_versions_follow_the_pre_one_point_zero_policy(self):
        self.assertEqual(module.next_version("0.4.2", {"fix"}), "0.4.3")
        self.assertEqual(module.next_version("0.4.2", {"fix", "feature"}), "0.5.0")
        self.assertEqual(module.next_version("0.4.2", {"breaking"}), "0.5.0")
        with self.assertRaisesRegex(module.CutError, "nothing user-visible"):
            module.next_version("0.4.2", {"internal"})
        with self.assertRaises(module.CutError):
            module.next_version("0.4", {"fix"})

    def test_cut_writes_one_ordered_section_and_consumes_fragments(self):
        self.fragment("zeta.fix.md", "Fixed a crash.\nIt no longer happens.\n")
        self.fragment("alpha.feature.md", "Added a feature.\n")
        self.fragment("old.breaking.md", "Removed the old spelling.\n")
        self.fragment("refactor.internal.md", "Reorganized internals.\n")
        self.assertEqual(module.cut(self.root, "Spring cleaning", apply=True), "0.5.0")
        self.assertEqual((self.root / "VERSION").read_text(), "0.5.0\n")
        changelog = (self.root / "CHANGELOG.md").read_text()
        self.assertEqual(changelog, "# Changelog\n\nPreamble.\n\n## 0.5.0 - Spring cleaning\n\n"
                         "- **Breaking:** Removed the old spelling.\n"
                         "- Added a feature.\n"
                         "- Fixed a crash.\n  It no longer happens.\n\n"
                         "## 0.4.2 - Earlier\n\n- Earlier change.\n")
        self.assertEqual(sorted(p.name for p in self.changes.iterdir()), ["README.md"])

    def test_dry_run_changes_nothing_and_bad_input_is_refused(self):
        self.fragment("small.fix.md", "Fixed it.\n")
        self.assertEqual(module.cut(self.root, "Fix", apply=False), "0.4.3")
        self.assertEqual((self.root / "VERSION").read_text(), "0.4.2\n")
        self.assertTrue((self.changes / "small.fix.md").exists())
        with self.assertRaisesRegex(module.CutError, "one-line title"):
            module.cut(self.root, "", apply=False)
        self.fragment("Bad.md", "text\n")
        with self.assertRaisesRegex(module.CutError, "name a fragment"):
            module.cut(self.root, "Fix", apply=False)
        (self.changes / "Bad.md").unlink()
        self.fragment("empty.fix.md", "  \n")
        with self.assertRaisesRegex(module.CutError, "user-facing text"):
            module.cut(self.root, "Fix", apply=False)

    def test_nothing_pending_is_refused(self):
        with self.assertRaisesRegex(module.CutError, "No changelog fragments"):
            module.cut(self.root, "Empty", apply=True)


if __name__ == "__main__":
    unittest.main()
