#!/usr/bin/env python3
"""Integral enums preserve exact numeric meaning through ordinary formatters."""
from lambda_pack_invocation_runtime_test import main as runtime_matrix

if __name__ == "__main__":
    runtime_matrix(names=("integral_enum_formatting", "integral_enum_formatting_panic"), cases={
        "integral_enum_formatting": [((), (0,
            "token creation failed: 9\nnamed 18446744073709551615\n", ""))],
        "integral_enum_formatting_panic": [((), (70, "",
            'integral_enum_formatting_panic.gti:4: runtime error[GTI-R0017]: '
            'panic: "Formatting failed: invalid format"\n'))]})
