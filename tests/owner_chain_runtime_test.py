#!/usr/bin/env python3
"""Checked recursive owner traversal selects storage and preserves cleanup."""

from pathlib import Path

from lambda_pack_invocation_runtime_test import main


if __name__ == "__main__":
    root = Path(__file__).resolve().parent.parent
    memory_lines = (root / "stdlib/std/detail/memory_core.gti").read_text().splitlines()
    failure_line = next(index + 1 for index, line in enumerate(memory_lines)
                        if "return gti_internal::unique_owner_borrow_mut(this.owner);" in line)
    main(names=("owner_chain",),
         fixture_paths={"owner_chain": root / "examples/ownership/owner-chain-references.gti"},
         cases={"owner_chain": [((), (0, "selected 42\nends 1 42\n"
                                      "destroy 1\ndestroy 2\ndestroy 42\n", "")),
                                (("past-end",), (70, "destroy 1\ndestroy 2\ndestroy 3\n",
                                  f"<std/detail/memory_core>:{failure_line}: "
                                  "runtime error[GTI-R0008]: empty_owner_access in dereference\n"))]},
         llvm_modes=True)
