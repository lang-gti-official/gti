#!/usr/bin/env python3
"""Source-defined mapped results retain native and cleanup behavior."""

import pathlib

from lambda_pack_invocation_runtime_test import main

if __name__ == "__main__":
    root = pathlib.Path(__file__).resolve().parents[1]
    main(names=("callable_result_types", "source_result_combinators",
                "native_callable_result_projection", "callable_reference_result_projection"),
         fixture_paths={"callable_result_types": root / "examples/167-callable-result-types.gti"},
         native_inputs_by_name={"native_callable_result_projection": [
             str(root / "tests/fixtures/native_callable_result_projection.cpp")]},
         cases={"callable_result_types": [((), (0, "mapped 42\n", ""))],
                "source_result_combinators": [((), (0, "42\n7\n", ""))]})
