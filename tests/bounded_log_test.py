#!/usr/bin/env python3
"""Exercise saved-log display bounds and preservation of raw evidence."""

from __future__ import annotations

import importlib.util
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


SCRIPT = Path(__file__).resolve().parents[1] / "scripts" / "bounded_log.py"
SPEC = importlib.util.spec_from_file_location("bounded_log", SCRIPT)
assert SPEC and SPEC.loader
LOG = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(LOG)


class BoundedLogTests(unittest.TestCase):
    def setUp(self):
        self.scratch = tempfile.TemporaryDirectory(prefix="gti-bounded-log-")
        self.addCleanup(self.scratch.cleanup)
        self.path = Path(self.scratch.name) / "run.log"

    def render(self, data: bytes, lines: int = 40, max_bytes: int = 8192) -> bytes:
        self.path.write_bytes(data)
        result = LOG.excerpt(self.path, lines, max_bytes)
        self.assertLessEqual(len(result), max_bytes)
        self.assertLessEqual(len(result.decode("utf-8").splitlines()), lines)
        self.assertEqual(self.path.read_bytes(), data)
        return result

    def test_small_complete_log(self):
        self.assertTrue(self.render(b"first\nlast\n").endswith(b"first\nlast\n"))

    def test_line_limit_keeps_latest_records(self):
        result = self.render(b"".join(f"record-{n}\n".encode() for n in range(1000)), 6)
        self.assertTrue(result.endswith(b"record-999\n"))
        self.assertNotIn(b"record-994\n", result)

    def test_giant_single_line(self):
        result = self.render(b'{"data":"' + b"x" * 1_000_000 + b'","end":true}\n', max_bytes=256)
        self.assertTrue(result.endswith(b'","end":true}\n'))

    def test_utf8_and_invalid_bytes_respect_display_budget(self):
        for data in ("😀".encode() * 10000, b"\xff" * 10000, "line\u2028".encode() * 1000):
            with self.subTest(data=data[:12]):
                self.render(data, lines=5, max_bytes=256)

    def test_empty_and_unterminated_logs(self):
        self.render(b"")
        self.assertTrue(self.render(b"unterminated").endswith(b"unterminated\n"))

    def test_missing_and_non_regular_inputs(self):
        with self.assertRaises(FileNotFoundError):
            LOG.excerpt(self.path, 40, 8192)
        with self.assertRaises(ValueError):
            LOG.excerpt(Path(self.scratch.name), 40, 8192)

    @unittest.skipUnless(hasattr(os, "mkfifo"), "FIFO is POSIX-specific")
    def test_fifo_is_rejected_without_waiting_for_a_writer(self):
        os.mkfifo(self.path)
        with self.assertRaises(ValueError):
            LOG.excerpt(self.path, 40, 8192)

    def test_invalid_limits_fail(self):
        self.path.write_bytes(b"data")
        for lines, maximum in ((1, 8192), (201, 8192), (40, 255), (40, 16385)):
            with self.subTest(lines=lines, maximum=maximum):
                with self.assertRaises(ValueError):
                    LOG.excerpt(self.path, lines, maximum)

    def test_cli_status_and_hard_ceilings(self):
        self.path.write_bytes(b"SUCCESS\n")
        result = subprocess.run([sys.executable, str(SCRIPT), str(self.path)], capture_output=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertTrue(result.stdout.endswith(b"SUCCESS\n"))
        for args in (("--lines", "201"), ("--max-bytes", "16385"), ("--lines", "invalid")):
            failed = subprocess.run([sys.executable, str(SCRIPT), str(self.path), *args], capture_output=True, timeout=10)
            self.assertEqual(failed.returncode, 2)
            self.assertLess(len(failed.stderr), 1024)
        missing = subprocess.run([sys.executable, str(SCRIPT), str(self.path.parent / "missing.log")], capture_output=True, timeout=10)
        self.assertEqual(missing.returncode, 1)
        self.assertLess(len(missing.stderr), 512)


if __name__ == "__main__":
    unittest.main()
