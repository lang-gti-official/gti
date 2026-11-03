#!/usr/bin/env python3
"""Counted-byte string search, slicing, and in-place editing."""

import os
import pathlib

from lambda_pack_invocation_runtime_test import main

if __name__ == "__main__":
    os.environ["GTI_STDLIB_PATH"] = str(pathlib.Path(__file__).resolve().parent.parent / "stdlib")
    main(names=("string_operations", "text_owner_relay"))
