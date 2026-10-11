#!/usr/bin/env python3
"""Constructor and staged-argument regressions exposed by map nodes."""

import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile

from lambda_pack_invocation_runtime_test import main as successful_cases
from harness_timeouts import scaled


def main():
    os.environ["GTI_STDLIB_PATH"] = str(Path(__file__).resolve().parent.parent / "stdlib")
    successful_cases(names=("associative_node_constructor",
                            "associative_include_only",
                            "borrowed_temporary_operator_receiver",
                            "dependent_carrier_representation",
                            "nested_owned_field_assignment",
                            "passive_nested_constructor_argument",
                            "associative_owning_node_constructor",
                            "associative_iterator_compound",
                            "associative_owned_argument"))
    compiler = Path(sys.argv[1]).resolve()
    source = Path(__file__).parent / "fixtures/associative_owned_argument_failure.gti"
    modes = [(f"{standard}-{level}", ["--std", standard, f"-{level}"], [])
             for standard in ("c++20", "c++23") for level in ("O0", "O1", "O3")]
    modes += [("no-elide", ["--std", "c++23", "-O0"], ["-fno-elide-constructors"]),
              ("sanitized", ["--std", "c++23", "-O1"],
               ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"])]
    selected = [sys.argv[index + 1] for index, value in enumerate(sys.argv)
                if value == "--mode"]
    if selected:
        modes = [mode for mode in modes if mode[0] in selected]
    environment = dict(os.environ, ASAN_OPTIONS="halt_on_error=1",
                       UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1")
    with tempfile.TemporaryDirectory(prefix="gti-associative-constructor-") as directory:
        for label, flags, native in modes:
            executable = Path(directory) / label
            command = [str(compiler), str(source), *flags, "-o", str(executable)]
            if native:
                command += ["--", *native]
            result = subprocess.run(command, capture_output=True, text=True,
                                    timeout=scaled(120), env=environment)
            assert result.returncode == 0, (label, result.stdout, result.stderr)
            result = subprocess.run([str(executable)], capture_output=True, text=True,
                                    timeout=scaled(30), env=environment)
            assert result.returncode == 70 and result.stdout == "cleaned\n", (label, result)
            assert re.fullmatch(r".+:\d+: runtime error\[GTI-R0006\]: "
                                r"numeric_conversion_out_of_range in numeric_cast\n",
                                result.stderr), (label, result.stderr)
            print(f"prepared class failure cleanup: {label} passed", flush=True)


if __name__ == "__main__":
    main()
