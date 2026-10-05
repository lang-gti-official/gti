#!/usr/bin/env python3
"""Inert typed/erased address conversion through the production MIR backend."""

from lambda_pack_invocation_runtime_test import main

if __name__ == "__main__":
    main(names=("raw_pointer_conversion",),
         cases={"raw_pointer_conversion": [((), (0, "", ""))]})
