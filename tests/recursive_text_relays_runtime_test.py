#!/usr/bin/env python3
"""Recursive relays of a local owner's text run in every mode."""
from lambda_pack_invocation_runtime_test import main

if __name__ == "__main__":
    output = "token text\n" * 7 + "literal\n"
    main(names=("recursive_text_relays",),
         cases={"recursive_text_relays": [((), (0, output, ""))]})
