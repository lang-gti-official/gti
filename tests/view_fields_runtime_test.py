#!/usr/bin/env python3
"""Values whose text fields hold views of a local owner run in every mode."""
from lambda_pack_invocation_runtime_test import main

if __name__ == "__main__":
    output = ("token text\n3\ntoken text\n2\ntoken text\n10\n50\n"
              "token text\nfixed\nliteral\n7\n")
    main(names=("view_fields",), cases={"view_fields": [((), (0, output, ""))]})
