#!/usr/bin/env python3
"""Disjoint scalar initialization preserves conditional enum field borrows."""

from lambda_pack_invocation_runtime_test import main


if __name__ == "__main__":
    main(names=("borrowed_enum_scalar_sibling",))
