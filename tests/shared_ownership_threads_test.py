#!/usr/bin/env python3
"""Concurrent retention and synchronized mutation through read-only owners."""

from lambda_pack_invocation_runtime_test import main


if __name__ == "__main__":
    main(("shared_ownership_threads", "shared_mutex_threads", "transfer_function_threads", "weak_ownership_threads"),
         execution_profile="concurrent", llvm_modes=True,
         cpp_unsupported="wrapping atomic read-modify-write")
