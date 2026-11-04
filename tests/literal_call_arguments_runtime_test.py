#!/usr/bin/env python3
"""Literal selection, integer ABI limits and temporary cleanup on LLVM O0/O3.

The shared backend policy retains the frozen C++ comparator when explicitly
selected; this feature adds no C++ backend implementation.
"""

from pathlib import Path

from lambda_pack_invocation_runtime_test import main


if __name__ == "__main__":
    root = Path(__file__).resolve().parents[1]
    main(names=("literal_call_arguments", "literal_call_arguments_example"),
         fixture_paths={"literal_call_arguments_example":
                        root / "examples/186-literal-call-arguments.gti"})
