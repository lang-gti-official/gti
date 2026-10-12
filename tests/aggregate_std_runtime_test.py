#!/usr/bin/env python3
"""The shipped aggregate-import example runs across native endpoint modes."""

from pathlib import Path

from lambda_pack_invocation_runtime_test import main


if __name__ == "__main__":
    example = Path(__file__).resolve().parent.parent / "examples/147-standard-library-import.gti"
    main(names=("aggregate_std",),
         fixture_paths={"aggregate_std": example},
         cases={"aggregate_std": [((), (0, "aggregate import: 42\n", ""))]})
