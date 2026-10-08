#!/usr/bin/env python3
"""Native observations distinguish the returned child from its protecting owner."""

import pathlib

from lambda_pack_invocation_runtime_test import main

if __name__ == "__main__":
    root = pathlib.Path(__file__).parent
    names = ("borrow_return_exact_referent", "a25-expected-ref-relay-wrong-referent",
             "a25-ref-return-from-expected-value", "borrow_owner_alias_field",
             "a58-ref-to-owner-field-pointee-via-local-ref",
             "a58-map-unique-ptr-value-deref", "borrow_nested_readonly",
             "borrow_nested_map", "a59-readonly-ref-into-mut-nested-container",
             "a59-map-at-then-element-ref", "borrow_projected_parameter_return")
    # The original map is deliberately empty: compilation must succeed, but
    # its checked at() call must retain the defined missing-key failure.
    map_lines = (root.parent / "stdlib/std/map.gti").read_text().splitlines()
    at_line = next(index for index, line in enumerate(map_lines)
                   if "mut V& at(K& key) mut" in line)
    missing_line = next(index + 1 for index in range(at_line, len(map_lines))
                        if "gti_internal::require_key" in map_lines[index])
    cases = {name: [((), (0, "2\n", ""))] for name in names[1:3]}
    cases["a59-map-at-then-element-ref"] = [((), (70, "",
        f"<std/map>:{missing_line}: runtime error[GTI-R0015]: "
        "key_not_found in key_lookup\n"))]
    main(names=names,
         fixture_paths={name: root / "coverage" / "reproducers" / f"{name}.gti"
                        for name in names if name.startswith(("a25-", "a58-", "a59-"))},
         cases=cases)
