#!/usr/bin/env python3
"""Check that both audit modes use one selected native compiler policy."""

import pathlib
import tempfile
from types import SimpleNamespace
import unittest
from unittest import mock

import screen


class NativePolicyTest(unittest.TestCase):
    def test_selected_policy(self):
        for mode in (screen.compare, screen.attribute):
            with self.subTest(mode=mode.__name__), tempfile.TemporaryDirectory() as root:
                arguments = SimpleNamespace(
                    gti=pathlib.Path("/selected/gti"), cxx="/selected/compiler with spaces",
                    std="c++20", level="3", runs=1, probes=["expected"],
                    output=pathlib.Path(root), runtime_include=pathlib.Path("/runtime/include"),
                    vendor_include=pathlib.Path("/vendor/include"),
                    runtime_library=pathlib.Path("/runtime/library.a"))
                commands = []

                def build(command):
                    commands.append(command)
                    if "--emit-cpp" in command:
                        source = "\n".join(old for replacements in screen.GUARDS.values()
                                           for old, _ in replacements)
                        pathlib.Path(command[command.index("-o") + 1]).write_text(source)

                with mock.patch.object(screen, "build", side_effect=build), \
                        mock.patch.object(screen, "run", return_value=(0.001, "42\n")):
                    mode(arguments)
                gti = commands[0]
                self.assertEqual(gti[gti.index("--cxx") + 1], arguments.cxx)
                self.assertEqual(gti[gti.index("--std") + 1], arguments.std)
                self.assertIn("-O3", gti)
                for native in commands[1:]:
                    self.assertEqual(native[0], arguments.cxx)
                    self.assertIn("-std=c++20", native)
                    self.assertIn("-O3", native)
                    for flag in screen.STRICT:
                        self.assertIn(flag, native)
                    if mode is screen.attribute:
                        self.assertIn(str(arguments.runtime_include), native)
                        self.assertIn(str(arguments.vendor_include), native)


if __name__ == "__main__":
    unittest.main()
