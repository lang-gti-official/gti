#!/usr/bin/env python3
"""Generated primary-template constructors are declared before native use."""

import pathlib

from lambda_pack_invocation_runtime_test import main


if __name__ == "__main__":
    memory = pathlib.Path(__file__).resolve().parent.parent / "stdlib/std/detail/memory_core.gti"
    read_sites = [line for line, text in enumerate(memory.read_text().splitlines(), 1)
                  if "return gti_internal::unique_owner_borrow(" in text]
    assert len(read_sites) == 2
    main(names=("generated_constructor_order", "generated_constructor_order_empty"),
         cases={"generated_constructor_order_empty": [
             ((), (70, "", f"<std/detail/memory_core>:{read_sites[1]}: "
                    "runtime error[GTI-R0008]: empty_owner_access in member_access\n"))]})
