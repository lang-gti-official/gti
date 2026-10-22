#!/usr/bin/env python3
"""Text read through returned references runs in every mode."""
from lambda_pack_invocation_runtime_test import main

if __name__ == "__main__":
    output = "token\ntoken\n5\nboxed\nfailure\n7\n"
    main(names=("text_reference_accessors",),
         cases={"text_reference_accessors": [((), (0, output, ""))]})
