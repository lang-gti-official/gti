#!/usr/bin/env python3
"""Forward-list cursor execution, checked lifetime exclusion, and failure cleanup."""

import argparse
import os
from pathlib import Path
import re
import statistics
import subprocess
import tempfile
import time


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("compiler", type=Path)
    parser.add_argument("--mode", action="append", default=[])
    parser.add_argument("--semantic-only", action="store_true")
    options = parser.parse_args()
    repository = Path(__file__).resolve().parent.parent
    compiler = options.compiler.resolve()
    environment = dict(os.environ, GTI_STDLIB_PATH=str(repository / "stdlib"),
                       ASAN_OPTIONS="halt_on_error=1",
                       UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1")

    def run(command, timeout=120):
        return subprocess.run(command, env=environment, capture_output=True,
                              text=True, timeout=timeout)

    modes = [(f"{standard}-{level}", [f"-{level}", "--std", standard], [])
             for standard in ("c++20", "c++23") for level in ("O0", "O1", "O3")]
    modes += [("no-elide", ["-O0", "--std", "c++23"], ["-fno-elide-constructors"]),
              ("sanitized", ["-O1", "--std", "c++23"],
               ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"])]
    if options.mode:
        unknown = set(options.mode) - {mode[0] for mode in modes}
        assert not unknown, f"unknown modes: {sorted(unknown)}"
        modes = [mode for mode in modes if mode[0] in options.mode]
    if options.semantic_only:
        modes = []

    with tempfile.TemporaryDirectory(prefix="gti-forward-list-iterator-") as temporary:
        directory = Path(temporary)
        for name, flags, native in modes:
            for fixture in ("forward_list_iterator_completion", "forward_list_iterator_failure",
                            "forward_list_raw_node_projection"):
                source = repository / "tests/fixtures" / f"{fixture}.gti"
                binary = directory / f"{fixture}-{name}"
                command = [str(compiler), str(source), *flags, "--keep-cpp", "-o", str(binary)]
                if native:
                    command += ["--", *native]
                result = run(command)
                assert result.returncode == 0, (name, fixture, result.stdout, result.stderr)
                emitted = binary.with_suffix(".gti.cpp").read_text()
                assert "// GTI verified-MIR body:" in emitted
                arguments = range(5) if fixture.endswith("failure") else range(1)
                for count in arguments:
                    result = run([str(binary), *(["fail"] * count)], timeout=30)
                    if fixture.endswith("failure"):
                        assert (result.returncode, result.stdout) == (70, "3\n2\n1\ncleaned\n"), result
                        assert re.fullmatch(
                            r".+:\d+: runtime error\[GTI-R0016\]: "
                            r"invalid_container_argument in container_argument\n", result.stderr), result.stderr
                    else:
                        assert (result.returncode, result.stdout, result.stderr) == (0, "", ""), result
                print(f"{fixture}: {name} passed", flush=True)

        source = repository / "tests/fixtures/forward_list_raw_node_projection.gti"
        mir_path = directory / "raw-projection.mir"
        result = run([str(compiler), str(source), "--emit-mir", "-o", str(mir_path)])
        assert result.returncode == 0, (result.stdout, result.stderr)
        mir = mir_path.read_text()
        assert " valid=1\n" in mir[:80], mir[:80]
        assert len(re.findall(r"(?m)^\s+i\d+ (?:call|call-input) .+raw-memory=1", mir)) >= 3
        assert re.search(r"projections=\[projection\(4;.+projection\(0;", mir)

        if not options.semantic_only:
            binary = directory / "forward-list-cost"
            started = time.monotonic()
            result = run([
                str(compiler), str(repository / "tests/fixtures/forward_list_iterator_cost.gti"),
                "-O3", "--std", "c++23", "--keep-cpp", "-o", str(binary), "--",
                str(repository / "tests/fixtures/stdlib_adoption_allocation_counts.cpp"),
                str(repository / "tests/fixtures/forward_list_iterator_cost.cpp"),
            ])
            assert result.returncode == 0, (result.stdout, result.stderr)
            build_seconds = time.monotonic() - started
            result = run([str(binary)], timeout=30)
            assert result.returncode == 0 and result.stderr == "", result
            observations = {128: [], 256: []}
            lines = result.stdout.splitlines()
            assert len(lines) == 10, result.stdout
            for line in lines:
                length, nanoseconds, allocations, checksum = map(int, line.split())
                assert length in observations and nanoseconds > 0 and allocations == 0, line
                assert checksum == length * (length - 1) // 2 * 20000, line
                observations[length].append(nanoseconds)
            assert all(len(samples) == 5 for samples in observations.values()), observations
            small = statistics.median(observations[128])
            large = statistics.median(observations[256])
            print(f"forward-list cost: 128={small}ns, 256={large}ns, "
                  f"ratio={large / small:.3f}, 0 traversal allocations; "
                  f"binary={binary.stat().st_size} bytes, "
                  f"C++={binary.with_suffix('.gti.cpp').stat().st_size} bytes, "
                  f"build={build_seconds:.3f}s", flush=True)

        prefix = """import std.forward_list;
class UserMutableField {
  mut int32_t value = 1;
public:
  void write() mut { this.value = 2; }
public:
  UserMutableField() = default;
};
struct UserRawOwner { mut UserMutableField field{};  public: UserRawOwner() = default; };
"""
        invalid = {
            "owner-mutation": "mut std::forward_list<int32_t> values{}; values.push_front(1); "
                              "auto cursor = values.begin(); values.pop_front(); if (*cursor == 0) return 1;",
            "owner-movement": "mut std::forward_list<int32_t> values{}; values.push_front(1); "
                              "auto cursor = values.begin(); auto moved = std::move(values); if (*cursor == 0) return 1;",
            "mutable-owner-read": "mut std::forward_list<int32_t> values{}; values.push_front(1); "
                                  "mut auto cursor = values.begin_mut(); [[discard]] values.front(); *cursor = 2;",
            "mutable-cursor-copy": "mut std::forward_list<int32_t> values{}; values.push_front(1); "
                                   "mut auto cursor = values.begin_mut(); auto copied = cursor; if (*copied == 0) return 1;",
            "readonly-element-write": "mut std::forward_list<int32_t> values{}; values.push_front(1); "
                                      "auto cursor = values.begin(); *cursor = 2;",
            "raw-field-needs-unsafe": "mut UserRawOwner owner{}; UserRawOwner* address = &owner; "
                                      "address->field.write();",
            "raw-field-constness": "mut UserRawOwner owner{}; const UserRawOwner* address = &owner; "
                                   "unsafe { address->field.write(); }",
        }
        expected = {
            "owner-mutation": "while a reference",
            "owner-movement": "overlapping borrow",
            "mutable-owner-read": "borrow",
            "mutable-cursor-copy": "copy",
            "readonly-element-write": "requires mutable access",
            "raw-field-needs-unsafe": "unsafe",
            "raw-field-constness": "requires a mutable receiver",
        }
        for name, body in invalid.items():
            source = directory / f"{name}.gti"
            source.write_text(prefix + f"int main() {{ {body} return 0; }}\n")
            result = run([str(compiler), str(source), "--emit-mir", "-o", str(directory / f"{name}.mir")])
            assert result.returncode == 65 and "GTI-S" in result.stderr and "GTI-B0001" not in result.stderr, (
                name, result.returncode, result.stdout, result.stderr)
            assert expected[name] in result.stderr, (name, result.stderr)
            print(f"forward-list lifetime {name}: rejected", flush=True)

        source = directory / "escape.gti"
        source.write_text(prefix + """
std::detail::forward_list_iterator<int32_t> escaped() {
  mut std::forward_list<int32_t> values{};
  values.push_front(1);
  return values.begin();
}
int main() { auto cursor = escaped(); return *cursor; }
""")
        result = run([str(compiler), str(source), "--emit-mir", "-o", str(directory / "escape.mir")])
        assert result.returncode == 65 and "GTI-S" in result.stderr and "GTI-B0001" not in result.stderr, result
        assert "borrow" in result.stderr, result.stderr
        print("forward-list lifetime local-owner-escape: rejected", flush=True)


if __name__ == "__main__":
    main()
