#!/usr/bin/env python3
"""Weak ownership cleanup and count overflow through a private stdlib alias.

The other shared-ownership failure fixtures are tests/ui/memory/shared_owner
cases; this one needs a disposable standard library exposing a private type.
"""

import argparse
import os
import shutil
from unittest.mock import patch
from pathlib import Path
import tempfile

from unique_class_allocation_runtime_test import run
from allocation_abort_runtime_test import ABORT_STATUS
from backend_modes import is_cpp_mode, native_modes


def main():
    modes = native_modes()
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("compiler", type=Path)
    parser.add_argument("--mode", action="append", choices=[mode[0] for mode in modes])
    arguments = parser.parse_args()
    compiler = arguments.compiler.resolve()
    fixtures = Path(__file__).resolve().parent / "fixtures"
    if arguments.mode:
        modes = [mode for mode in modes if mode[0] in arguments.mode]
    with tempfile.TemporaryDirectory(prefix="gti-shared-failure-") as temporary:
        # The overflow probes deliberately inspect a private control block.
        # A disposable interface exposes a test alias; shipped API stays private.
        library = Path(temporary) / "stdlib"
        shutil.copytree(fixtures.parent.parent / "stdlib", library)
        with (library / "std/memory.gti").open("a", encoding="utf-8") as output:
            output.write("\nexport using gti_test_shared_control = std::detail::shared_control;\n")
        weak_source = Path(temporary) / "weak_ownership_failure.gti"
        weak_source.write_text((fixtures / "weak_ownership_failure.gti").read_text().replace(
            "std::detail::shared_control", "gti_test_shared_control"), encoding="utf-8")
        for name, flags, native in modes:
            weak_binary = Path(temporary) / f"weak-{name}"
            with patch.dict(os.environ, {"GTI_STDLIB_PATH": str(library)}):
                built = run([str(compiler), str(weak_source),
                             *flags, "-o", str(weak_binary)])
            assert built.returncode == 0, (name, built.stdout, built.stderr)
            for args, status, output, code in [
                    ([], 70, "7\n99\n", "GTI-R0002"),
                    (["overflow-weak"], ABORT_STATUS, "", "GTI-R0011"),
                    (["overflow-lock", "overflow"], ABORT_STATUS, "", "GTI-R0011")]:
                result = run([str(weak_binary), *args])
                assert result.returncode == status and result.stdout == output, (name, args, result)
                assert f"runtime error[{code}]" in result.stderr, (name, args, result)
            print(f"weak ownership cleanup and overflow: {name} passed", flush=True)

if __name__ == "__main__":
    main()
