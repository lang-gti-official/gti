#!/usr/bin/env python3
"""Exact range fields, indexed and returned views, text members through
checked `operator->`, and allocated values."""

from pathlib import Path
from lambda_pack_invocation_runtime_test import main


if __name__ == "__main__":
    root = Path(__file__).parent
    reproducers = root / "coverage" / "reproducers"
    originals = ("a28-map-range-for-key-view",
                 "a29-method-on-returned-string-view",
                 "a29-string-view-size-on-dynamic-view",
                 "a28-range-element-field-ref-method",
                 "a28-range-element-field-overloaded-operator",
                 "a34-view-relay-through-subscript",
                 "a56-make-unique-same-type",
                 "a56-make-unique-string-from-string")
    # An empty optional's read-only checked access step reports its own
    # defined failure before the text member runs.
    optional_lines = (root.parent / "stdlib/std/optional.gti").read_text().splitlines()
    value_line = next(index for index, line in enumerate(optional_lines)
                      if line.strip() == "T& value() {")
    empty_line = next(index + 1 for index in range(value_line, len(optional_lines))
                      if "fail_empty_owner" in optional_lines[index])
    arrow_text = "7\n0\n1\n7\n0\n7\n10\n13\n13\n0\n4\n"
    main(names=("range_field_access", "indexed_view_relay",
                "existing_value_allocation", "returned_view_observers",
                "arrow_text_members", *originals),
         fixture_paths={name: reproducers / (name + ".gti") for name in originals},
         cases={"indexed_view_relay": [((), (0, "alpha\nbeta\ngamma\ndelta\n", ""))],
                "returned_view_observers": [((), (0, "5\n", ""))],
                "arrow_text_members": [
                    ((), (0, arrow_text, "")),
                    (("empty",), (70, arrow_text,
                                  f"<std/optional>:{empty_line}: runtime error[GTI-R0008]: "
                                  "empty_owner_access in member_access\n"))]})
