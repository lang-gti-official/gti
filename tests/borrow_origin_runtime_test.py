#!/usr/bin/env python3
"""Native observations distinguish the returned child from its protecting owner."""

import pathlib

from lambda_pack_invocation_runtime_test import main

if __name__ == "__main__":
    root = pathlib.Path(__file__).parent
    names = ("borrow_return_exact_referent", "a25-expected-ref-relay-wrong-referent",
             "a25-ref-return-from-expected-value", "borrow_owner_alias_field",
             "a58-ref-to-owner-field-pointee-via-local-ref",
             "a58-map-unique-ptr-value-deref")
    main(names=names,
         fixture_paths={name: root / "coverage" / "reproducers" / f"{name}.gti"
                        for name in names if name.startswith(("a25-", "a58-"))},
         cases={name: [((), (0, "2\n", ""))] for name in names[1:3]})
