#!/usr/bin/env python3
"""Live-slot/prefix/lane iteration: execution, bounds, cleanup and owner loans."""

import os
import pathlib
import re
import subprocess
import sys
import tempfile


def run(command, *, timeout=180):
    return subprocess.run(command, text=True, capture_output=True, timeout=timeout)


def reject_invalid(compiler, root):
    includes = "#include <std/memory>\n#include <std/resource>\n#include <std/simd>\n"
    storage = """std::allocator allocator{};
  mut auto result = allocator.try_storage<int>(std::size_t(3));
  if (!result) { return 1; }
  mut auto values = std::move(result).value();
  values.construct(std::size_t(1), 42);
"""
    arena = """mut auto result = std::try_make_arena<int>(std::size_t(3));
  if (!result) { return 1; }
  mut auto values = std::move(result).value();
  [[discard]] values.try_insert(42);
"""
    pool = """mut auto result = std::try_make_pool<int>(std::size_t(3));
  if (!result) { return 1; }
  mut auto values = std::move(result).value();
  [[discard]] values.try_insert(42);
"""
    resource = """std::process_resource process{};
  unsafe {
    auto binding = std::bind_resource(process);
    mut std::resource_storage<int, std::process_resource> values{binding};
    if (!values.try_allocate(std::size_t(3))) { return 1; }
    values.emplace(std::size_t(1), 42);
"""
    cases = {
        "storage-clear": (storage + "auto cursor = values.begin(); values.clear(); return *cursor;", "GTI-S2017"),
        "storage-release": (storage + "auto cursor = values.begin(); values.release(); return *cursor;", "GTI-S2017"),
        "storage-destroy": (storage + "auto cursor = values.begin(); values.destroy(std::size_t(1)); return *cursor;", "GTI-S2017"),
        "storage-exclusive-copy": (storage + "auto cursor = values.begin_mut(); auto copied = cursor; return *copied;", "GTI-S2003"),
        "arena-reset": (arena + "auto cursor = values.begin(); values.reset(); return *cursor;", "GTI-S2017"),
        "arena-insert": (arena + "auto cursor = values.begin(); [[discard]] values.try_insert(7); return *cursor;", "GTI-S2017"),
        "pool-erase": (pool + "auto cursor = values.begin(); values.erase(std::size_t(0)); return *cursor;", "GTI-S2017"),
        "pool-clear": (pool + "auto cursor = values.begin(); values.clear(); return *cursor;", "GTI-S2017"),
        "resource-release": (resource + "auto cursor = values.begin(); values.release(); return *cursor; }", "GTI-S2017"),
        "resource-destroy": (resource + "auto cursor = values.begin(); values.destroy(std::size_t(1)); return *cursor; }", "GTI-S2017"),
        "resource-reverse-release": (resource + "auto cursor = values.rbegin(); values.release(); return *cursor; }", "GTI-S2017"),
        "arena-reverse-reset": (arena + "auto cursor = values.rbegin_mut(); values.reset(); return *cursor;", "GTI-S2017"),
        "simd-owner-move": ("mut std::simd<float, 4> values{2.0}; auto cursor = values.begin(); auto moved = std::move(values); return int(*cursor);", "GTI-S2017"),
        "simd-overlap": ("mut std::simd<float, 4> values{2.0}; auto cursor = values.begin_mut(); [[discard]] values.reduce(); return int(*cursor);", "GTI-S2017"),
        "mask-exclusive-copy": ("mut std::simd_mask<4> values{}; auto cursor = values.begin_mut(); auto copied = cursor; return *copied ? 0 : 1;", "GTI-S2003"),
        "mask-shared-write": ("mut std::simd_mask<4> values{}; mut auto cursor = values.begin(); *cursor = true; return 0;", "GTI-S2002"),
        "mask-reverse-exclusive-copy": ("mut std::simd_mask<4> values{}; auto cursor = values.rbegin_mut(); auto copied = cursor; return *copied ? 0 : 1;", "GTI-S2003"),
    }
    for name, (body, code) in cases.items():
        source = root / f"{name}.gti"
        source.write_text(includes + "int main() {\n" + body + "\n}\n")
        checked = run([str(compiler), str(source), "--emit-mir", "-o", str(root / f"{name}.mir")])
        assert checked.returncode == 65 and f"error[{code}]" in checked.stderr, (name, checked.stderr)
        assert "GTI-B0001" not in checked.stderr, (name, checked.stderr)
    print(f"storage/lane iteration: {len(cases)} invalid owner/access cases rejected", flush=True)


def main():
    if len(sys.argv) != 2:
        raise SystemExit("usage: storage_lane_iterator_runtime_test.py <optimized-gti>")
    repository = pathlib.Path(__file__).resolve().parent.parent
    os.environ["GTI_STDLIB_PATH"] = str(repository / "stdlib")
    compiler = pathlib.Path(sys.argv[1]).resolve()
    fixtures = repository / "tests" / "fixtures"
    modes = [(f"{standard}-{level}", ["--std", standard, f"-{level}"])
             for standard in ("c++20", "c++23") for level in ("O0", "O1", "O3")]
    modes.append(("sanitized", ["--std", "c++23", "-O1", "--",
                                "-fsanitize=address,undefined", "-fno-omit-frame-pointer"]))
    with tempfile.TemporaryDirectory(prefix="gti-storage-lane-iteration-") as directory:
        root = pathlib.Path(directory)
        reject_invalid(compiler, root)
        for name, flags in modes:
            for fixture in ("storage_lane_iterator_completion", "storage_lane_iterator_failure"):
                binary = root / f"{fixture}-{name}"
                built = run([str(compiler), str(fixtures / f"{fixture}.gti"), "--keep-cpp", "-o", str(binary), *flags])
                assert built.returncode == 0, (fixture, name, built.stdout, built.stderr)
                generated = binary.with_suffix(".gti.cpp").read_text()
                assert "// GTI verified-MIR body:" in generated, (fixture, name)
                if fixture.endswith("completion"):
                    completed = run([str(binary)])
                    assert (completed.returncode, completed.stdout, completed.stderr) == (0, "", ""), (name, completed)
                else:
                    for argc in range(1, 19):
                        failed = run([str(binary), *(["fail"] * (argc - 1))])
                        code = "GTI-R0007" if argc in (1, 8, 12) else "GTI-R0016"
                        assert failed.returncode == 70 and failed.stdout == "20\n10\n", (name, argc, failed)
                        assert re.search(rf"runtime error\[{code}\]:", failed.stderr), (name, argc, failed.stderr)
                        assert "AddressSanitizer" not in failed.stderr and "UndefinedBehaviorSanitizer" not in failed.stderr, failed.stderr
                print(f"{fixture}: {name} passed", flush=True)


if __name__ == "__main__":
    main()
