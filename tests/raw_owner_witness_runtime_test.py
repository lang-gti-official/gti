#!/usr/bin/env python3
"""Checked raw-owner borrowing across optimization and native C++ modes."""

from lambda_pack_invocation_runtime_test import main


if __name__ == "__main__":
    main(names=("raw_owner_witness",),
         cases={"raw_owner_witness": [((), (0, "", ""))]})
