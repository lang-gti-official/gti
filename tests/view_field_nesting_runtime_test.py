#!/usr/bin/env python3
"""Dependent values nested in fields and payloads run in every mode."""
from lambda_pack_invocation_runtime_test import main

if __name__ == "__main__":
    output = ("nested text\nnested text\n4\nnested text\n6\n11\nnested text\n"
              "bad input\n3\n0\nliteral\n")
    main(names=("view_field_nesting",),
         cases={"view_field_nesting": [((), (0, output, ""))]})
