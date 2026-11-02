#!/usr/bin/env python3
"""Consuming ranges transfer move-only values and retain exact cleanup."""
from lambda_pack_invocation_runtime_test import main

if __name__ == "__main__":
    main(names=("consuming_range",), llvm_modes=True)
