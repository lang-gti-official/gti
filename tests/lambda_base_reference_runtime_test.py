#!/usr/bin/env python3
"""Lambda invocations bind derived class objects to base-reference parameters."""

from lambda_pack_invocation_runtime_test import main


if __name__ == "__main__":
    main(names=("lambda_base_reference_arguments",), cases={
        "lambda_base_reference_arguments": [((), (0, "7\n18\n", ""))],
    })
