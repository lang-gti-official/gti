#!/usr/bin/env python3
"""Inline storage starts/ends unrelated object lifetimes without allocation."""
import pathlib
from lambda_pack_invocation_runtime_test import main

if __name__ == "__main__":
    root = pathlib.Path(__file__).resolve().parent.parent
    main(names=("uninitialized_carrier", "inline_owner_example", "inline_pool_example",
                "bitwise_range_relocation"), cases={
        "uninitialized_carrier": [((), (0, "42\n7\n19\n19\n", ""))],
        "inline_owner_example": [((), (0, "42\nrelocated\n43\n", ""))],
        "inline_pool_example": [((), (0, "1\n2\n4\n", ""))],
        "bitwise_range_relocation": [((), (0, "", ""))],
    }, fixture_paths={
        "inline_owner_example": root / "examples/125-manual-object-lifetimes.gti",
        "inline_pool_example": root / "examples/128-inline-object-pool.gti",
    })
