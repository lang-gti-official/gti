#!/usr/bin/env python3
"""Native ownership regressions exposed by associative container entries."""

from lambda_pack_invocation_runtime_test import main


if __name__ == "__main__":
    main(names=("associative_entry_borrow", "associative_slot_replacement",
                "associative_default_comparator"))
