#!/usr/bin/env python3
"""Raw destructive relocation transfers one cleanup obligation without copying."""
from lambda_pack_invocation_runtime_test import main

if __name__ == "__main__":
    main(names=("object_relocation",), cases={
        "object_relocation": [((), (0, "payload\n24\n23\n", ""))],
    })
