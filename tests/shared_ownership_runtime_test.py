#!/usr/bin/env python3
"""Explicit shared-owner retention, borrowing, and final-release cleanup."""

from lambda_pack_invocation_runtime_test import main


if __name__ == "__main__":
    main(("shared_ownership", "shared_ownership_upcast"), {
        "shared_ownership": [((), (0, "1\n7\n2\n8\n3\n9\n", ""))],
        "shared_ownership_upcast": [((), (0, "7\n8\n", ""))],
    })
