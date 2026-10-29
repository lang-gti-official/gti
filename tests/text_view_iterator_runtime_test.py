#!/usr/bin/env python3
"""Builtin text-view normalization, checked cursors, and shell lifetimes."""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile
import statistics
import time


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("compiler", type=Path)
    parser.add_argument("--mode", action="append", default=[])
    parser.add_argument("--semantic-only", action="store_true")
    parser.add_argument("--cost", action="store_true")
    options = parser.parse_args()
    root = Path(__file__).resolve().parent.parent
    compiler = options.compiler.resolve()
    environment = dict(os.environ, GTI_STDLIB_PATH=str(root / "stdlib"),
                       ASAN_OPTIONS="halt_on_error=1",
                       UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1")

    def run(command, timeout=180):
        return subprocess.run([str(item) for item in command], env=environment,
                              text=True, capture_output=True, timeout=timeout)

    modes = [(f"{std}-{level}", [f"-{level}", "--std", std], [])
             for std in ("c++20", "c++23") for level in ("O0", "O1", "O3")]
    modes += [("no-elide", ["-O0", "--std", "c++23"], ["-fno-elide-constructors"]),
              ("sanitized", ["-O1", "--std", "c++23"],
               ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"])]
    if options.mode:
        assert set(options.mode) <= {name for name, _, _ in modes}
        modes = [mode for mode in modes if mode[0] in options.mode]
    if options.semantic_only:
        modes = []
    with tempfile.TemporaryDirectory(prefix="gti-text-view-iteration-") as directory:
        output = Path(directory)
        for name, flags, native in modes:
            cases = [(root / "tests/fixtures/text_view_iterator_completion.gti", []),
                     (root / "tests/fixtures/text_view_iterator_module.gti", []),
                     (root / "tests/fixtures/text_view_iterator_failure.gti",
                      [root / "tests/fixtures/text_view_iterator_support.cpp"]),
                     (root / "examples/146-counted-text-view-iteration.gti", [])]
            for source, support in cases:
                executable = output / f"{source.stem}-{name}"
                command = [compiler, source, *flags, "--progress=never", "--keep-cpp", "-o", executable]
                if support or native:
                    command += ["--", *support, *native]
                result = run(command)
                assert result.returncode == 0, (name, source.name, result.stdout, result.stderr)
                cpp = executable.with_suffix(".gti.cpp").read_text()
                assert "// GTI verified-MIR body:" in cpp
                if source.stem == "text_view_iterator_failure":
                    for count in range(12):
                        failed = run([executable, *(["failure"] * count)], timeout=30)
                        expected_status = 0 if count == 11 else 70
                        # A second failure during cleanup terminates through
                        # the emergency envelope immediately, without running
                        # any remaining destructors (the established contract).
                        expected_output = ("3\n" if count == 9 else
                                           "2\n1\n" if count <= 5 or count == 7 else "3\n2\n1\n")
                        assert (failed.returncode, failed.stdout) == (expected_status, expected_output), (name, count, failed)
                        if expected_status == 0:
                            assert failed.stderr == "", (name, count, failed.stderr)
                        else:
                            assert "runtime error[GTI-R" in failed.stderr, failed.stderr
                        assert ("GTI-R0014" in failed.stderr) == (count == 9), (name, count, failed.stderr)
                        assert not any(word in failed.stderr for word in
                                       ("AddressSanitizer", "UndefinedBehaviorSanitizer", "double-free")), failed.stderr
                else:
                    passed = run([executable], timeout=30)
                    assert (passed.returncode, passed.stdout, passed.stderr) == (0, "", ""), (name, source.name, passed)
                print(f"{source.stem}: {name} passed", flush=True)

        invalid = {
            "owner-invalidation": ('import std.string;\nint main() { mut std::string owner{"abc"}; '
                'std::string_view view = owner.view(); auto cursor = view.begin(); owner.clear(); return int(*cursor); }', "GTI-S2017"),
            "argument-invalidates-owner": ('import std.string;\nstd::size_t erase(mut std::string& owner) { '
                'owner.clear(); return std::size_t(0); } int main() { mut std::string owner{"abc"}; '
                'std::string_view view = owner.view(); auto cursor = view.iterator_at(erase(owner)); return int(*cursor); }', "GTI-S2017"),
            "readonly-element": ('import std.string_view;\nint main() { std::string_view view = "abc"; '
                'mut auto cursor = view.begin(); *cursor = \'z\'; return 0; }', "GTI-S2002"),
            "escaped-view-shell": ('import std.string;\nimport std.string_view;\nstd::detail::text_view_iterator<0> escape(std::string& owner) { '
                'std::string_view view = owner.view(); return view.begin(); } int main() { return 0; }', "GTI-S2045"),
            "temporary-view-shell": ('import std.string_view;\nint main() { auto cursor = "abc".begin(); return int(*cursor); }', "GTI-S2017"),
            "ordinary-value-temporary-reference": ('import std.string_view;\nint main() { std::string_view view = "abc"; '
                'auto cursor = view.begin(); char& character = *cursor; return int(character); }', "GTI-S2017"),
            "value-iterator-reference-callback": ('import std.string;\nimport std.algorithm;\n'
                'class RefCounter { public: void operator()(char& value) mut {}  public: RefCounter() = default; }; '
                'int main() { std::string_view view = "abc"; '
                'auto visits = std::for_each(view.begin(), view.end(), RefCounter()); return 0; }', "GTI-S2029"),
            "value-range-mutable-reference": ('import std.string_view;\nint main() { std::string_view view = "abc"; '
                'for (mut char& character : view) {} return 0; }', "GTI-S2083"),
            "mutable-value-dereference": ('class ValueCursor { public: int operator*() mut { return 1; }  public: ValueCursor() = default; }; '
                'int main() { return 0; }', "GTI-S2022"),
            "value-arrow": ('class ValueCursor { public: int operator->() { return 1; }  public: ValueCursor() = default; }; '
                'int main() { return 0; }', "GTI-S2022"),
            "borrowed-value-dereference": ('struct Borrowed { int& value; }; '
                'class ValueCursor { public: Borrowed operator*();  public: ValueCursor() = default; }; int main() { return 0; }', "GTI-S2022"),
            "move-only-value-dereference": ('import std.string;\nclass ValueCursor { public: '
                'std::string operator*() { return std::string("owned"); }  public: ValueCursor() = default; }; int main() { return 0; }', "GTI-S2022"),
        }
        for name, (source, code) in invalid.items():
            path = output / f"{name}.gti"
            path.write_text(source)
            emitted = output / f"{name}.mir"
            result = run([compiler, path, "--emit-mir", "-o", emitted])
            assert result.returncode != 0 and code in result.stderr, (name, result.stdout, result.stderr)
            assert "GTI-B0001" not in result.stderr and not emitted.exists(), (name, result.stderr)
            print(f"{name}: rejected", flush=True)
        if options.cost:
            executable = output / "text-view-cost"
            started = time.monotonic()
            result = run([compiler, root / "tests/fixtures/text_view_iterator_cost.gti",
                          "-O3", "--std", "c++23", "--progress=never", "--keep-cpp", "-o", executable,
                          "--", root / "tests/fixtures/stdlib_adoption_allocation_counts.cpp",
                          root / "tests/fixtures/forward_list_iterator_cost.cpp"])
            assert result.returncode == 0, (result.stdout, result.stderr)
            built_seconds = time.monotonic() - started
            measured = run([executable], timeout=60)
            assert measured.returncode == 0 and not measured.stderr, measured
            rows = [tuple(map(int, line.split())) for line in measured.stdout.splitlines()]
            assert len(rows) == 10 and all(length in (128, 256) and allocations == 0 and
                checksum == 97 * length * 20000 for length, _, allocations, checksum in rows), rows
            elapsed = {length: statistics.median(row[1] for row in rows if row[0] == length)
                       for length in (128, 256)}
            ratio = elapsed[256] / elapsed[128]
            # This is an asymptotic/cost smoke check, not a claimed speedup.
            # Five interleaved trials allow normal scheduling noise, but reject
            # quadratic index-walk iteration and all traversal allocations.
            assert ratio < 3.5, (ratio, rows)
            print(f"text-view cost: 128={elapsed[128]}ns 256={elapsed[256]}ns ratio={ratio:.3f} "
                  f"allocations=0 binary={executable.stat().st_size}B "
                  f"cpp={executable.with_suffix('.gti.cpp').stat().st_size}B "
                  f"native-build={built_seconds:.3f}s", flush=True)


if __name__ == "__main__":
    main()
