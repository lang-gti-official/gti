#!/usr/bin/env python3
"""Shared accessor authority composes with independent owned worker outputs."""

from lambda_pack_invocation_runtime_test import main

if __name__ == "__main__":
    main(names=("shared_getter_worker_loans",), execution_profile="concurrent")
