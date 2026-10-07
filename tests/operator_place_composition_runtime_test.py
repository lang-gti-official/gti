#!/usr/bin/env python3
"""Exact returned places compose through updates across the full native matrix."""
from pathlib import Path
from lambda_pack_invocation_runtime_test import main

if __name__ == "__main__":
    root = Path(__file__).parent
    names = ("operator_place_composition", "operator_place_stdlib", "a26", "a27", "a46_compound", "a46_read")
    main(names=names, fixture_paths={
        "operator_place_composition": root / "fixtures/operator_place_composition.gti",
        "operator_place_stdlib": root / "fixtures/operator_place_stdlib.gti",
        "a26": root / "coverage/reproducers/a26-overloaded-deref-store-reads-owner.gti",
        "a27": root / "coverage/reproducers/a27-compound-assign-overloaded-deref.gti",
        "a46_compound": root / "coverage/reproducers/a46-chained-subscript-compound.gti",
        "a46_read": root / "coverage/reproducers/a46-chained-subscript-self-read.gti",
    })
