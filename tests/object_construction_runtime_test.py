#!/usr/bin/env python3
"""Source-owned raw slots preserve final-address construction and cleanup."""

import pathlib
import tempfile

from lambda_pack_invocation_runtime_test import main


if __name__ == "__main__":
    fixture = pathlib.Path(__file__).parent / "fixtures/object_construction.gti"
    lines = fixture.read_text().splitlines()
    constructor = next(index for index, line in enumerate(lines, 1)
                       if "20 / constructor_divisor" in line)
    destructor = next(index for index, line in enumerate(lines, 1)
                      if "1 / this.divisor" in line)
    argument = next(index for index, line in enumerate(lines, 1)
                    if "constructor_divisor / argument_divisor" in line)
    body = next(index for index, line in enumerate(lines, 1) if "1 / zero" in line)

    def failure(line):
        return (f"{fixture.name}:{line}: runtime error[GTI-R0002]: "
                "division_by_zero in integer_division\n")

    double = (f"{fixture.name}:{destructor}: runtime error[GTI-R0014]: "
              "failure_during_cleanup; primary [GTI-R0002] at "
              f"{fixture.name}:{body}; secondary [GTI-R0002]\n")
    raw_cases = [
        ((), (0, "7\n20\n10\n7\n20\n11\n900\n", "")),
        (("constructor",), (70, "10\n900\n", failure(constructor))),
        (("destructor", "failure"), (70, "7\n20\n10\n900\n", failure(destructor))),
        (("argument", "preparation", "failure"), (70, "10\n900\n", failure(argument))),
        (("body", "failure", "cleanup", "success"),
         (70, "7\n20\n10\n900\n", failure(body))),
        (("body", "failure", "cleanup", "also", "fails"),
         # The body failure is already active when Anchor's destructor fails.
         # Stop before its fields (20/10) and the outer region (900), while
         # preserving the exact body-primary and destructor-secondary sites.
         (70, "7\n", double)),
    ]
    # Run the same selected-constructor and cleanup contract through an exact
    # destination owner. Keep the raw path as an independent control.
    with tempfile.TemporaryDirectory(prefix="gti-owned-construction-") as temporary:
        owned = pathlib.Path(temporary) / "object_construction_owned.gti"
        source = fixture.read_text()
        original = "std::construct_at(std::typed_address<T>(this.region.address()), args...);"
        assert source.count(original) == 1
        owned.write_text(source.replace(original,
            "std::construct_owned_at(std::typed_address<T>(this.region.address()), this, args...);"))
        owned_cases = [(arguments, (code, output, diagnostic.replace(fixture.name, owned.name)))
                       for arguments, (code, output, diagnostic) in raw_cases]
        # A reference-bearing record exercises borrowed cleanup without
        # requiring the deferred dynamic-text destructor publication contract.
        referenced = pathlib.Path(temporary) / "object_construction_reference.gti"
        reference_source = owned.read_text()
        reference_replacements = {
            "  int divisor;": "  int divisor; int& borrowed;",
            "Anchor(Piece first, int constructor_divisor, int destructor_divisor)":
                "Anchor(Piece first, int constructor_divisor, int destructor_divisor, int& input)",
            "divisor(destructor_divisor) {}": "divisor(destructor_divisor), borrowed(input) {}",
            "    std::println(7);":
                '    if (this.borrowed != 42) { std::panic("invalid borrowed cleanup payload"); } std::println(7);',
            "  mut TestSlot<Anchor> slot{};": '  int owner = 42; mut TestSlot<Anchor> slot{};',
            "  slot.construct(Piece(10), constructor_divisor / argument_divisor,":
                "  slot.construct<Piece, int, int, int&>(Piece(10), constructor_divisor / argument_divisor,",
            "                 destructor_divisor);": "                 destructor_divisor, owner);",
            "  slot.construct(Piece(11), 1, 1);":
                "  slot.construct<Piece, int, int, int&>(Piece(11), 1, 1, owner);",
        }
        for old, new in reference_replacements.items():
            assert reference_source.count(old) == 1, old
            reference_source = reference_source.replace(old, new)
        assert len(reference_source.splitlines()) == len(lines)
        referenced.write_text(reference_source)
        reference_cases = [(arguments, (code, output, diagnostic.replace(fixture.name, referenced.name)))
                           for arguments, (code, output, diagnostic) in raw_cases]
        array = fixture.parent / "borrowed_array_cleanup.gti"
        array_failure_line = next(index for index, line in enumerate(
            array.read_text().splitlines(), 1) if "input / divisor" in line)
        array_cases = [
            ((), (0, "22\n11\n", "")),
            (("fail",), (70, "11\n",
                f"{array.name}:{array_failure_line}: runtime error[GTI-R0002]: "
                "division_by_zero in integer_division\n")),
        ]
        vector = fixture.parent / "borrowed_vector_cleanup.gti"
        assert vector.read_text().splitlines()[array_failure_line - 1].strip() == "int checked = input / divisor;"
        vector_cases = [(arguments, (code, output, diagnostic.replace(array.name, vector.name)))
                        for arguments, (code, output, diagnostic) in array_cases]
        main(names=("object_construction", "object_construction_owned",
                    "object_construction_reference", "borrowed_array_cleanup",
                    "borrowed_vector_cleanup"),
             fixture_paths={"object_construction_owned": owned,
                            "object_construction_reference": referenced,
                            "borrowed_array_cleanup": array,
                            "borrowed_vector_cleanup": vector},
             cases={"object_construction": raw_cases,
                    "object_construction_owned": owned_cases,
                    "object_construction_reference": reference_cases,
                    "borrowed_array_cleanup": array_cases,
                    "borrowed_vector_cleanup": vector_cases})
