#!/usr/bin/env python3
"""Inline storage starts/ends unrelated object lifetimes without allocation."""
from lambda_pack_invocation_runtime_test import main

if __name__ == "__main__":
    main(names=("uninitialized_carrier",), cases={
        "uninitialized_carrier": [((), (0, "42\n7\n19\n19\n", ""))],
    })
