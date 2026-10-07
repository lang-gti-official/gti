#!/usr/bin/env python3
"""Stored/forwarded pointer slots and real native pointer-array writeback."""

from pathlib import Path

from lambda_pack_invocation_runtime_test import main

if __name__ == "__main__":
    root = Path(__file__).resolve().parent.parent
    main(names=("nested_raw_pointers",),
         fixture_paths={"nested_raw_pointers":
                        root / "examples/124-nested-raw-pointers.gti"})
