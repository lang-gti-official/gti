#!/usr/bin/env python3
"""Execute the trusted source-policy fixture through verified MIR and native C++."""
import os
import pathlib
import subprocess
import sys
import tempfile


def main():
    emitter, cxx, runtime = map(pathlib.Path, sys.argv[1:4])
    root = pathlib.Path(__file__).resolve().parent.parent
    policy = root / "stdlib/std/detail/memory_storage.gti"
    fixture = root / "tests/fixtures/source_sparse_slots.gti"

    def failure(source, fragment, code, reason):
        line = next(i for i, text in enumerate(source.read_text().splitlines(), 1)
                    if fragment in text)
        name = "<std/detail/memory_storage>" if source == policy else "<prelude>"
        return f"{name}:{line}: runtime error[GTI-R{code:04}]: {reason}\n"

    cases = [((), 0, "3\n2\n1\n", "")]
    for count, fragment, detail, output in (
        (1, "require_vacant_storage(!", "duplicate_construction", "1\n"),
        (2, "require_live_storage(this.initialized", "uninitialized_access", "1\n"),
        (3, "count <= this.slots", "relocation_capacity", "1\n"),
        (4, "require_relocation_source(this.initialized(index))", "invalid_relocation_source", "1\n"),
        (5, "require_relocation_destination(!destination.initialized", "occupied_relocation_destination", "9\n1\n"),
    ):
        cases.append((tuple("arg" for _ in range(count)), 70, output,
                      failure(policy, fragment, 10, f"invalid_storage_state in {detail}")))
    cases.append((tuple("arg" for _ in range(6)), 70, "3\n2\n1\n",
                  failure(fixture, "1 / this.divisor", 2,
                          "division_by_zero in integer_division")))
    destructor = next(i for i, line in enumerate(fixture.read_text().splitlines(), 1)
                      if "1 / this.divisor" in line)
    cases.append((tuple("arg" for _ in range(7)), 70, "3\n2\n",
                  f"<prelude>:{destructor}: runtime error[GTI-R0014]: "
                  f"failure_during_cleanup; primary [GTI-R0002] at <prelude>:{destructor}; "
                  "secondary [GTI-R0002]\n"))
    modes = [(standard, level, []) for standard in ("c++20", "c++23")
             for level in ("O0", "O1", "O3")]
    modes += [("c++23", "O0", ["-fno-elide-constructors"]),
              ("c++23", "O1", ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"])]
    env = dict(os.environ, ASAN_OPTIONS="halt_on_error=1",
               UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1")
    with tempfile.TemporaryDirectory(prefix="gti-source-sparse-") as directory:
        scratch = pathlib.Path(directory)
        for index, (standard, level, extra) in enumerate(modes):
            cpp, binary = scratch / f"{index}.cpp", scratch / f"{index}"
            emitted = subprocess.run([str(emitter), str(cpp), standard, level],
                                     capture_output=True, text=True, timeout=120)
            assert emitted.returncode == 0, (emitted.stdout, emitted.stderr)
            built = subprocess.run([str(cxx), f"-std={standard}", f"-{level}",
                                    "-fno-fast-math", "-ffp-contract=off",
                                    "-D__gti_strict_ieee754=1",
                                    "-I" + str(root / "runtime/include"),
                                    str(cpp), str(runtime), "-pthread", *extra,
                                    "-o", str(binary)],
                                   capture_output=True, text=True, timeout=120)
            assert built.returncode == 0, (built.stdout, built.stderr)
            for arguments, code, output, error in cases:
                run = subprocess.run([str(binary), *arguments], env=env,
                                     capture_output=True, text=True, timeout=30)
                assert (run.returncode, run.stdout, run.stderr) == (code, output, error), (
                    standard, level, extra, arguments, run)
            print(f"source sparse storage: {standard} {level} {extra} passed", flush=True)


if __name__ == "__main__":
    main()
