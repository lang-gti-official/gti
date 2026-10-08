#!/usr/bin/env python3
"""Audited conditional ownership survives concrete MIR/native lowering."""
import pathlib
from lambda_pack_invocation_runtime_test import main

if __name__ == "__main__":
    root = pathlib.Path(__file__).resolve().parent.parent
    main(names=("conditional_owner",), cases={
        "conditional_owner": [((), (0, "", ""))],
    }, fixture_paths={
        "conditional_owner": root / "examples/126-conditional-thread-capabilities.gti",
    })
