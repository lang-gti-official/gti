#!/usr/bin/env python3
"""Search/comparison/output algorithms and cross-container checked traversal."""
import os
import pathlib

from lambda_pack_invocation_runtime_test import main

if __name__ == "__main__":
    os.environ["GTI_STDLIB_PATH"] = str(pathlib.Path(__file__).resolve().parent.parent / "stdlib")
    # Compile each concrete search-suite instantiation separately: together they
    # can exhaust the per-compile deadline on CI, before native compilation.
    main(names=("algorithm_search", "algorithm_search_list", "algorithm_search_deque",
                "algorithm_search_array", "algorithm_mutation", "algorithm_boundaries",
                "algorithm_user_types"))
