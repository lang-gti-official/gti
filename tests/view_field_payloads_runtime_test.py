#!/usr/bin/env python3
"""Text published through payload-enum fields runs in every mode."""
from lambda_pack_invocation_runtime_test import main

if __name__ == "__main__":
    output = ("owned text\n10\nowned text\n3\nowned text\nowned text\n7\n"
              "owned text\nowned text\n10\n10\nowned text\nstatic\n")
    main(names=("view_field_payloads",),
         cases={"view_field_payloads": [((), (0, output, ""))]})
