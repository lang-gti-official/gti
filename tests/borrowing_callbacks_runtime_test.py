#!/usr/bin/env python3
"""Checked reference capture environments preserve identity without allocations."""

from lambda_pack_invocation_runtime_test import main


if __name__ == "__main__":
    main(names=("borrowing_callbacks", "checked_callable_erasure"))
