#!/usr/bin/env python3
"""Source-defined checked numeric parsing across native configurations."""

import os
import pathlib

from lambda_pack_invocation_runtime_test import main

if __name__ == "__main__":
    os.environ["GTI_STDLIB_PATH"] = str(pathlib.Path(__file__).resolve().parent.parent / "stdlib")
    main(names=("charconv_integer", "charconv_float", "charconv_owned_arguments"))
