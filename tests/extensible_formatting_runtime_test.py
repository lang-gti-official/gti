#!/usr/bin/env python3
"""Source-defined formatters and exact signature contracts use verified MIR."""
from lambda_pack_invocation_runtime_test import main as runtime_matrix

if __name__ == "__main__":
    runtime_matrix(names=("extensible_formatting", "exact_member_signatures"),
                   cases={"extensible_formatting": [((), (0, "ID=002a Record(7)\n", ""))]})
