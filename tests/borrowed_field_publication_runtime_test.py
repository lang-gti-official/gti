#!/usr/bin/env python3
"""Stored text reads, projected writes and deferred generic field publication."""

from lambda_pack_invocation_runtime_test import main

if __name__ == "__main__":
    main(names=("borrowed_field_publication",),
         cases={"borrowed_field_publication": [((), (0, "", ""))]},
         llvm_modes=True)
