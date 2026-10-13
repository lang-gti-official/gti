#!/usr/bin/env python3
"""Concrete enum packs retain values and owner cleanup across native modes."""

import pathlib
import sys

from pack_parameter_runtime_test import report, run_cases


if __name__ == "__main__":
    if len(sys.argv) != 2:
        raise SystemExit("usage: enum_pack_runtime_test.py <gti>")
    run_cases(pathlib.Path(sys.argv[1]).resolve(), {
        "pack_enum_values": [(0, "", "")],
        "pack_enum_lifetimes": [
            (0, "0\n1\n9\n", ""),
            (70, "0\n1\n9\n", report("pack_enum_lifetimes", 29)),
        ],
    })
