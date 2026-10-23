#!/usr/bin/env python3
"""References into values over a local owner's text are observed in place."""
from lambda_pack_invocation_runtime_test import main

if __name__ == "__main__":
    name = "dependent_reference_results"
    main(names=(name,), cases={name: [
        ((), (0, "owned text\n10\nliteral\nliteral\n", "")),
    ]})
