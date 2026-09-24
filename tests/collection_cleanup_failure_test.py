#!/usr/bin/env python3
"""Reverse live-element cleanup retains first and secondary failure records."""

import pathlib
import sys
import tempfile

from unique_class_allocation_runtime_test import run


def main():
    if len(sys.argv) != 2:
        raise SystemExit("usage: collection_cleanup_failure_test.py <gti>")
    compiler = pathlib.Path(sys.argv[1]).resolve()
    source = pathlib.Path(__file__).parent / "fixtures/collection_cleanup_failure.gti"
    lines = source.read_text().splitlines()
    site = lambda text: next(i for i, line in enumerate(lines, 1) if text in line)
    element = site("element_failure =")
    body = site("return 17 / zero")
    suffix = site("suffix_failure =")
    memory = source.parents[2] / "stdlib/std/memory.gti"
    destroy_site = next(i for i, line in enumerate(memory.read_text().splitlines(), 1)
                        if "gti_internal::storage_destroy(this.data, index);" in line)
    vector = memory.with_name("vector.gti")
    erase_site = next(i for i, line in enumerate(vector.read_text().splitlines(), 1)
                     if "gti_internal::prefix_storage_erase(this.data, index);" in line)

    def ordinary(line):
        return (f"{source.name}:{line}: runtime error[GTI-R0002]: "
                "division_by_zero in integer_division\n")

    def emergency(primary, secondary=element):
        return (f"{source.name}:{secondary}: runtime error[GTI-R0014]: "
                "failure_during_cleanup; primary [GTI-R0002] at "
                f"{source.name}:{primary}; secondary [GTI-R0002]\n")

    cases = [
        (0, "2\n1\n9\n", ""),
        (70, "2\n1\n9\n", ordinary(element)),
        (70, "2\n1\n9\n", ordinary(body)),
        (70, "2\n", emergency(body)),
        (70, "2\n1\n", emergency(element)),
        (0, "2\n1\n9\n", ""),
        (70, "2\n", emergency(body)),
        (70, "2\n1\n9\n", ordinary(element)),
        (70, "2\n", emergency(body)),
        (70, "2\n1\n3\n9\n", ordinary(element)),
        (70, "2\n", emergency(body)),
        (70, "2\n1\n9\n", ordinary(body)),
        (70, "9\n", ordinary(body)),
        (70, "2\n", emergency(body)),
    ]
    cases.extend([
        (0, "2\n1\n9\n", ""),  # Explicit pop, successful element cleanup.
        (70, "2\n1\n9\n", ordinary(element)),
        (70, "2\n1\n", emergency(element)),
        (0, "2\n1\n9\n", ""),  # Explicit sparse-slot destruction.
        (70, "2\n1\n9\n", ordinary(element)),
        (70, "2\n1\n", emergency(element)),
        (70, "2\n1\n9\n", ordinary(element)),  # Direct class pop.
        (70, "2\n1\n3\n9\n", ordinary(element)),  # Nested collection pop.
        (70, "2\n1\n9\n", ordinary(element)),  # Vector clear uses pop.
        (70, "2\n1\n9\n", ordinary(element)),  # Sparse clear uses destroy.
        (0, "9\n", ""),  # Public empty pop is a no-op.
        (70, "2\n1\n9\n", f"<std/memory>:{destroy_site}: runtime error[GTI-R0010]: "
         "invalid_storage_state in uninitialized_access\n"),
        (0, "2\n4\n3\n1\n9\n", ""),  # Interior erase + append.
        (70, "2\n3\n1\n9\n", ordinary(element)),
        (70, "2\n3\n", emergency(element, suffix)),  # Distinct suffix failure site.
        (70, "2\n3\n1\n", emergency(element)),  # Failure in caller prefix cleanup.
        (0, "2\n4\n3\n1\n9\n", ""),  # Direct-class relocation.
        (70, "2\n3\n1\n9\n", ordinary(element)),
        (70, "1\n3\n2\n9\n", ordinary(element)),  # First element failure.
        (70, "3\n2\n1\n9\n", ordinary(element)),  # Last element failure.
        (70, "3\n2\n1\n9\n", f"<std/vector>:{erase_site}: runtime error[GTI-R0007]: "
         "index_out_of_bounds in private_storage\n"),
        (0, "1\n4\n3\n2\n9\n", ""),
        (0, "3\n4\n2\n1\n9\n", ""),
        (70, "2\n3\n1\n9\n", ordinary(element)),  # Nested collection erase.
        (0, "2\n3\n1\n9\n", ""),
        (70, "2\n3\n1\n9\n", ordinary(element)),  # Generated owning aggregate.
        (0, "2\n4\n3\n1\n9\n", ""),
        (0, "2\n4\n3\n1\n9\n", ""),  # Owning inherited element.
        (70, "2\n3\n1\n9\n", ordinary(element)),
    ])
    inherited = source.with_name("inherited_collection_cleanup_failure.gti")
    inherited_lines = inherited.read_text().splitlines()
    primary = next(i for i, line in enumerate(inherited_lines, 1) if "checked(7 / zero)" in line)
    secondary = next(i for i, line in enumerate(inherited_lines, 1) if "secondary =" in line)
    inherited_expected = (
        70, "element-cleanup\n",
        f"{inherited.name}:{secondary}: runtime error[GTI-R0014]: "
        "failure_during_cleanup; primary [GTI-R0002] at "
        f"{inherited.name}:{primary}; secondary [GTI-R0002]\n")
    modes = [(f"{standard}-{opt}", ["--std", standard, f"-{opt}"], [])
             for standard in ("c++20", "c++23") for opt in ("O0", "O1", "O3")]
    modes.append(("sanitized", ["--std", "c++23", "-O1"],
                  ["--", "-fsanitize=address,undefined", "-fno-omit-frame-pointer"]))
    with tempfile.TemporaryDirectory(prefix="gti-collection-cleanup-") as directory:
        for name, flags, native in modes:
            binary = pathlib.Path(directory) / name
            command = [str(compiler), str(source), *flags, "--keep-cpp",
                       "-o", str(binary), *native]
            built = run(command)
            assert built.returncode == 0, (command, built.stdout, built.stderr)
            generated = binary.with_suffix(".gti.cpp").read_text()
            assert ".destroy_elements_with_failure(" in generated
            for phase, expected in enumerate(cases):
                result = run([str(binary), *(["phase"] * phase)])
                actual = (result.returncode, result.stdout, result.stderr)
                assert actual == expected, (name, phase, actual, expected)
            inherited_binary = pathlib.Path(directory) / f"{name}-inherited"
            built = run([str(compiler), str(inherited), *flags, "-o", str(inherited_binary), *native])
            assert built.returncode == 0, (name, built.stdout, built.stderr)
            result = run([str(inherited_binary)])
            assert (result.returncode, result.stdout, result.stderr) == inherited_expected, (
                name, result, inherited_expected)
            print(f"collection cleanup: {name} passed ({len(cases) + 1} cases)", flush=True)


if __name__ == "__main__":
    main()
