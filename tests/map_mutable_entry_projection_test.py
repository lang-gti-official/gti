#!/usr/bin/env python3
"""Dictionary mutation exposes projections, never replaceable owning entries."""

import os
import pathlib
import re
import statistics
import subprocess
import sys
import tempfile
import time

from native_fixture_build import build_fixture


def run(command):
    return subprocess.run(command, text=True, capture_output=True, timeout=180)


def reject_invalid(compiler, fixtures, directory):
    includes = "#include <std/map>\n#include <std/unordered_map>\n#include <std/string>\n#include <std/utility>\n"
    cases = {}
    for kind in ("map", "unordered_map"):
        setup = f"mut std::{kind}<int,int> values{{}}; [[discard]] values.insert(1,10); mut auto cursor = values.begin_mut();\n"
        statements = {
            "replace-entry": "*cursor = std::detail::map_entry<int,int>(2,20);",
            "move-entry": "std::detail::map_entry<int,int> moved = std::move(*cursor);",
            "swap-entry": "mut std::detail::map_entry<int,int> other{2,20}; std::swap(*cursor, other);",
            "exchange-entry": "[[discard]] std::exchange(*cursor, std::detail::map_entry<int,int>(2,20));",
            "write-key": "cursor->key() = 2;",
            "write-first": "cursor->first() = 2;",
            "clear-owner": "mut auto& projected = *cursor; values.clear(); return projected.mapped();",
            "move-owner": "mut auto& projected = *cursor; auto moved = std::move(values); return projected.mapped();",
            "advance-with-mapped": "mut int& mapped = cursor->mapped(); ++cursor; return mapped;",
            "advance-with-key": "int& key = cursor->key(); ++cursor; return key;",
            "copy-exclusive-view": "auto view = *cursor;",
            "move-projected-view": "mut auto view = std::move(*cursor);",
            "swap-projected-views": f"mut std::{kind}<int,int> other{{}}; [[discard]] other.insert(2,20); mut auto second = other.begin_mut(); std::swap(*cursor,*second);",
            "exchange-projected-views": f"mut std::{kind}<int,int> other{{}}; [[discard]] other.insert(2,20); mut auto second = other.begin_mut(); [[discard]] std::exchange(*cursor,std::move(*second));",
        }
        for name, statement in statements.items():
            cases[f"{kind}-{name}"] = includes + "int main() {\n" + setup + statement + "\n return 0; }\n"
        cases[f"{kind}-move-owning-key"] = includes + f"""int main() {{
  mut std::{kind}<std::string,int> values{{}};
  [[discard]] values.insert(std::string("one"),10);
  mut auto cursor = values.begin_mut();
  std::string moved = std::move(cursor->key());
  return 0;
}}
"""
    user = (fixtures / "projected_record_iteration.gti").read_text()
    prefix = user[:user.index("int main()")]
    cases["user-record-replace"] = prefix + """int main() {
  mut Record value{1,10}; mut RecordCursor cursor{value};
  *cursor = Record(2,20); return 0;
}
"""
    cases["user-record-readonly-identity"] = prefix + """int main() {
  mut Record value{1,10}; mut RecordCursor cursor{value};
  cursor->key() = 2; return 0;
}
"""
    for name, source in cases.items():
        path = directory / f"{name}.gti"
        path.write_text(source)
        checked = run([str(compiler), str(path), "--emit-mir", "-o", str(path.with_suffix(".mir"))])
        assert checked.returncode == 65 and "error[GTI-S" in checked.stderr, (name, checked.stdout, checked.stderr)
        assert "GTI-B0001" not in checked.stderr, (name, checked.stderr)
    print(f"mutable entry projection: {len(cases)} key/owner/access bypasses rejected", flush=True)


def measure_projection(compiler, fixtures, directory):
    binary = directory / "map-entry-cost"
    started = time.monotonic()
    built = build_fixture(
        [str(compiler), str(fixtures / "map_mutable_entry_cost.gti"),
         "--std", "c++23", "-O3"], binary,
        native_sources=[fixtures / "stdlib_adoption_allocation_counts.cpp",
                        fixtures / "storage_lane_iterator_cost.cpp"],
        standard="c++23", optimization="O3", capture_output=True, text=True,
        timeout=180)
    assert built.returncode == 0, (built.stdout, built.stderr)
    seconds = time.monotonic() - started
    completed = run([str(binary)])
    assert completed.returncode == 0 and completed.stderr == "", completed
    measurements = {(family, length): [] for family in (1, 2) for length in (128, 256)}
    rows = completed.stdout.splitlines()
    assert len(rows) == 20, completed.stdout
    for row in rows:
        family, length, elapsed, allocations, checksum = map(int, row.split())
        expected = length * 2000 * (2 * 100 + 2000 + 1) // 2 + length * (length - 1) * 2000 // 2
        assert allocations == 0 and checksum == expected and elapsed > 0, row
        measurements[(family, length)].append(elapsed)
    assert all(len(samples) == 5 for samples in measurements.values()), measurements
    for family, label in ((1, "ordered"), (2, "hash")):
        small = statistics.median(measurements[(family, 128)])
        large = statistics.median(measurements[(family, 256)])
        print(f"{label} mapped projection: 128={small}ns, 256={large}ns, ratio={large / small:.3f}; "
              "0 update allocations, complete final keys/values checked", flush=True)
    print(f"mapped projection cost: binary={binary.stat().st_size} bytes, "
          f"C++={binary.with_suffix('.gti.cpp').stat().st_size} bytes, build={seconds:.3f}s; "
          "bounded cost control, no speedup claim", flush=True)


def main():
    if len(sys.argv) not in (2, 3) or (len(sys.argv) == 3 and sys.argv[2] not in ("--negative-only", "--cost-only")):
        raise SystemExit("usage: map_mutable_entry_projection_test.py <optimized-gti> [--negative-only | --cost-only]")
    repository = pathlib.Path(__file__).resolve().parent.parent
    os.environ["GTI_STDLIB_PATH"] = str(repository / "stdlib")
    compiler = pathlib.Path(sys.argv[1]).resolve()
    fixtures = repository / "tests" / "fixtures"
    with tempfile.TemporaryDirectory(prefix="gti-map-entry-projection-") as temporary:
        directory = pathlib.Path(temporary)
        if len(sys.argv) == 3 and sys.argv[2] == "--cost-only":
            measure_projection(compiler, fixtures, directory)
            return
        reject_invalid(compiler, fixtures, directory)
        if len(sys.argv) == 3:
            return
        modes = [(f"{standard}-{level}", ["--std", standard, f"-{level}"])
                 for standard in ("c++20", "c++23") for level in ("O0", "O1", "O3")]
        modes.append(("sanitized", ["--std", "c++23", "-O1", "--",
                                    "-fsanitize=address,undefined", "-fno-omit-frame-pointer"]))
        for mode, flags in modes:
            for fixture in ("map_mutable_entry_projection", "projected_record_iteration",
                            "associative_map_insert_result", "unordered_map_insert_result",
                            "associative_map_iteration", "unordered_map_iteration", "map_mutable_entry_failure"):
                binary = directory / f"{fixture}-{mode}"
                built = run([str(compiler), str(fixtures / f"{fixture}.gti"), "--keep-cpp", "-o", str(binary), *flags])
                assert built.returncode == 0, (fixture, mode, built.stdout, built.stderr)
                assert "// GTI verified-MIR body:" in binary.with_suffix(".gti.cpp").read_text(), (fixture, mode)
                if fixture.endswith("failure"):
                    for argc in range(1, 9):
                        failed = run([str(binary), *(["fail"] * (argc - 1))])
                        assert failed.returncode == 70 and failed.stdout == "20\n10\n", (mode, argc, failed)
                        assert re.search(r"runtime error\[GTI-R0016\]:", failed.stderr), (mode, argc, failed.stderr)
                        assert "AddressSanitizer" not in failed.stderr and "UndefinedBehaviorSanitizer" not in failed.stderr, failed.stderr
                else:
                    completed = run([str(binary)])
                    assert (completed.returncode, completed.stdout, completed.stderr) == (0, "", ""), (fixture, mode, completed)
                print(f"{fixture}: {mode} passed", flush=True)
        measure_projection(compiler, fixtures, directory)


if __name__ == "__main__":
    main()
