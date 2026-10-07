#!/usr/bin/env python3
"""Sparse slot publication, address stability and exact failure cleanup."""

import pathlib

from lambda_pack_invocation_runtime_test import main


if __name__ == "__main__":
    fixture = pathlib.Path(__file__).parent / "fixtures/storage_final_address.gti"
    constructor_line = next(i + 1 for i, line in enumerate(fixture.read_text().splitlines())
                            if "12 / divisor" in line)
    memory = pathlib.Path(__file__).parent.parent / "stdlib/std/memory.gti"
    lines = memory.read_text().splitlines()
    vacancy_line = next(i + 1 for i, line in enumerate(lines)
                        if "require_vacant_storage(!this.initialized(index))" in line)
    bounds_line = next(i + 1 for i, line in enumerate(lines)
                       if "index_bounds_check(index, this.slots)" in line)
    constructor_failure = (f"{fixture.name}:{constructor_line}: runtime error[GTI-R0002]: "
                           "division_by_zero in integer_division\n")
    duplicate_failure = (f"<std/memory>:{vacancy_line}: runtime error[GTI-R0010]: "
                         "invalid_storage_state in duplicate_construction\n")
    bounds_failure = (f"<std/memory>:{bounds_line}: runtime error[GTI-R0007]: "
                      "index_out_of_bounds in private_storage\n")
    release = fixture.with_name("storage_release.gti")
    release_panic_line = next(i + 1 for i, line in enumerate(release.read_text().splitlines())
                              if 'std::panic("package cleanup failed")' in line)
    release_failure = (f"{release.name}:{release_panic_line}: runtime error[GTI-R0017]: "
                       'panic: "package cleanup failed"\n')
    main(names=(fixture.stem, release.stem, "owning_storage_view",
                "storage_fixed_array_emplacement"), cases={fixture.stem: [
        ((), (0, "30\n20\n10\n90\n30\n20\n13\n", "")),
        (("constructor",), (70, "20\n10\n90\n", constructor_failure)),
        (("duplicate", "slot"), (70, "11\n90\n30\n20\n10\n", duplicate_failure)),
        (("outside", "slot", "range"), (70, "12\n90\n30\n20\n10\n", bounds_failure)),
        (("fail", "after", "live", "slot"),
         (70, "20\n14\n90\n30\n20\n10\n", constructor_failure)),
        (("release", "allocation", "before", "scope", "exit"),
         (0, "30\n20\n10\n30\n20\n13\n77\n90\n", "")),
        (("owner", "constructor", "allocation", "partial", "rollback", "failure"),
         (70, "20\n16\n30\n20\n15\n90\n", constructor_failure)),
    ], release.stem: [
        ((), (0, "30\n10\n77\n90\n", "")),
        (("panic",), (70, "30\n10\n90\n", release_failure)),
    ]}, fixture_paths={"owning_storage_view": fixture.parent.parent.parent /
                       "examples/114-owning-storage-view.gti"})
