#!/usr/bin/env python3
"""Exact generic callable-pack and dependent-clause programs use verified MIR."""

import pathlib

from lambda_pack_invocation_runtime_test import main


if __name__ == "__main__":
    root = pathlib.Path(__file__).resolve().parent
    paths = {
        "function_item_pack_contracts": root / "fixtures/function_item_pack_contracts.gti",
        "dependent_clause": root / "coverage/reproducers/a19-trailing-clause-dependent-compound.gti",
        "source_concepts_example": root.parent / "examples/33-source-concepts.gti",
        "function_items_example": root.parent / "examples/80-function-items.gti",
    }
    main(names=tuple(paths), fixture_paths=paths)
