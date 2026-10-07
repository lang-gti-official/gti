#!/usr/bin/env python3
"""Structural relocation preserves values, empty slots, and exactly-once drops."""

import pathlib

from lambda_pack_invocation_runtime_test import main


if __name__ == "__main__":
    memory = pathlib.Path(__file__).parent.parent / "stdlib/std/memory.gti"
    lines = memory.read_text().splitlines()
    relocate = next(i + 1 for i, line in enumerate(lines)
                    if "storage_relocate(this.data" in line)
    read = next(i + 1 for i, line in enumerate(lines)
                if "return gti_internal::storage_read(this.data" in line)
    def failure(line, detail, stdout=""):
        return (70, stdout, f"<std/memory>:{line}: runtime error[GTI-R0010]: "
                f"invalid_storage_state in {detail}\n")
    main(names=("structural_relocation",), cases={"structural_relocation": [
        ((), (0, "71\n82\n81\nrelocation passed\n", "")),
        (("capacity",), failure(relocate, "relocation_capacity")),
        (("occupied", "destination"), failure(relocate, "occupied_relocation_destination")),
        (("uninitialized", "source", "tail"), failure(relocate, "invalid_relocation_source")),
        (("consumed", "source", "cannot", "read"), failure(read, "uninitialized_access")),
        (("owned", "occupied", "late", "slot", "transaction"),
         failure(relocate, "occupied_relocation_destination", "201\n104\n102\n101\n")),
        (("owned", "uninitialized", "late", "source", "slot", "transaction"),
         failure(relocate, "invalid_relocation_source", "104\n102\n101\n")),
    ]})
