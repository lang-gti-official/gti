#!/usr/bin/env python3
"""Selected-target C numeric families through the production MIR backend."""

from pathlib import Path

from lambda_pack_invocation_runtime_test import main

if __name__ == "__main__":
    root = Path(__file__).resolve().parent.parent
    main(names=("native_c_scalars",),
         fixture_paths={"native_c_scalars": root / "examples/123-native-c-scalars.gti"})
