#!/usr/bin/env python3
"""Text kept through conversions, field relays and returns runs in every mode."""
from lambda_pack_invocation_runtime_test import main

if __name__ == "__main__":
    output = ("owned text\nowned text\nowned text\nowned text\n10\n"
              "owned text\nbad input\n9\n0\nowned text\n7\nbad input\n"
              "bad input\n")
    main(names=("view_field_conversions",),
         cases={"view_field_conversions": [((), (0, output, ""))]})
