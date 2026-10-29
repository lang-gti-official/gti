#!/usr/bin/env python3
"""Abstract-only generic virtual calls retain the checked ABI declaration."""

from pathlib import Path

from lambda_pack_invocation_runtime_test import main


if __name__ == "__main__":
    memory = Path(__file__).parent.parent / "stdlib/std/detail/memory_core.gti"
    lines = memory.read_text().splitlines()
    read = [i for i, line in enumerate(lines, 1)
            if "return gti_internal::unique_owner_borrow(" in line]
    mutable = [i for i, line in enumerate(lines, 1)
               if "return gti_internal::unique_owner_borrow_mut(" in line]
    assert len(read) == len(mutable) == 2

    def empty(site):
        return (f"<std/detail/memory_core>:{site}: runtime error[GTI-R0008]: "
                "empty_owner_access in member_access\n")

    main(names=("abstract_generic_failure_contract",), cases={
        "abstract_generic_failure_contract": [
            ((), (70, "", empty(read[1]))),
            (("mutable",), (70, "", empty(mutable[1]))),
        ],
    })
