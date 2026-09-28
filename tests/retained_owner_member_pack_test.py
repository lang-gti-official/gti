#!/usr/bin/env python3
"""Retained native owners preserve member-pack arity and exact cleanup."""

from lambda_pack_invocation_runtime_test import main


if __name__ == "__main__":
    name = "retained_owner_member_pack"
    main(names=(name,), cases={name: [
        ((), (0, "8\n8\n8\n8\n8\n1\n8\n3\n2\n", "")),
    ]})
