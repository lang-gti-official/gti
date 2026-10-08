#!/usr/bin/env python3
"""Exact range fields, indexed and returned views, and allocated values."""

from pathlib import Path
from lambda_pack_invocation_runtime_test import main


if __name__ == "__main__":
    reproducers = Path(__file__).parent / "coverage" / "reproducers"
    originals = ("a28-map-range-for-key-view",
                 "a29-method-on-returned-string-view",
                 "a29-string-view-size-on-dynamic-view",
                 "a28-range-element-field-ref-method",
                 "a28-range-element-field-overloaded-operator",
                 "a34-view-relay-through-subscript",
                 "a56-make-unique-same-type",
                 "a56-make-unique-string-from-string")
    main(names=("range_field_access", "indexed_view_relay",
                "existing_value_allocation", "returned_view_observers",
                *originals),
         fixture_paths={name: reproducers / (name + ".gti") for name in originals},
         cases={"indexed_view_relay": [((), (0, "alpha\nbeta\ngamma\ndelta\n", ""))],
                "returned_view_observers": [((), (0, "5\n", ""))]})
