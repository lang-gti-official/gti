#!/usr/bin/env python3
"""JSON object projections preserve extent, immutable keys and document loans."""

import argparse
import os
from pathlib import Path
import subprocess
import tempfile

from case_jobs import configured_jobs, positive_jobs, run_cases
from contiguous_iterator_completion_test import MODES
from native_fixture_build import build_fixture


def negative_sources():
    headers = """#include <std/json>
#include <std/string>
#include <std/span>
#include <std/paired_span>
"""
    setup = """mut auto object = std::json::value::object();
[[discard]] object.set("key", std::json::value::boolean(true));
"""
    bodies = {
        "key-write": 'auto keys = object.keys(); keys[std::size_t(0)] = std::string("different"); return 0;',
        "key-character-write": "auto keys = object.keys(); keys[std::size_t(0)][std::size_t(0)] = 'x'; return 0;",
        "shared-value-write": "auto values = object.values(); values[std::size_t(0)] = std::json::value::boolean(false); return 0;",
        "member-key-write": 'auto members = object.members(); mut auto cursor = members.begin(); cursor->first() = std::string("different"); return 0;',
        "member-value-write": "auto members = object.members(); mut auto cursor = members.begin(); cursor->second() = std::json::value::boolean(false); return 0;",
        "key-owner-replacement": "auto keys = object.keys(); object = std::json::value::object(); return int(keys.size());",
        "member-owner-edit": 'auto members = object.members(); [[discard]] object.set("another", false); return int(members.size());',
        "exclusive-owner-read": "mut auto values = object.values_mut(); auto count = object.object_size(); return int(values.size() + count);",
        "exclusive-copy": "mut auto values = object.values_mut(); auto copy = values; return int(copy.size());",
        "exclusive-alias": "mut auto values = object.values_mut(); auto keys = object.keys(); return int(values.size() + keys.size());",
        "cursor-child": "auto members = object.members(); mut auto cursor = members.begin(); auto& entry = *cursor; ++cursor; return int(entry.first().size());",
    }
    sources = {name: headers + "int main() {\n" + setup + body + "\n}\n"
               for name, body in bodies.items()}
    sources["local-view-escape"] = headers + """
std::span<std::json::value> escaped() {
  mut auto object = std::json::value::object();
  return object.values();
}
int main() { return 0; }
"""
    sources["local-member-escape"] = headers + """
std::paired_span<std::string, std::json::value> escaped() {
  mut auto object = std::json::value::object();
  return object.members();
}
int main() { return 0; }
"""
    return sources


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("compiler", type=Path)
    parser.add_argument("--mode", action="append", choices=[mode[0] for mode in MODES])
    parser.add_argument("--jobs", type=positive_jobs, default=configured_jobs())
    options = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    compiler = options.compiler.resolve()
    environment = dict(os.environ, GTI_STDLIB_PATH=str(root / "stdlib"),
                       ASAN_OPTIONS="halt_on_error=1",
                       UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1")
    modes = [mode for mode in MODES if not options.mode or mode[0] in options.mode]
    fixtures = ("json_object_iteration", "json_object_iteration_failure")
    with tempfile.TemporaryDirectory(prefix="gti-json-object-iteration-") as raw:
        output = Path(raw)

        def build(case):
            fixture, label, flags, native = case
            binary = output / f"{fixture}-{label}"
            command = [str(compiler), str(root / "tests/fixtures" / f"{fixture}.gti"),
                       *flags, "--progress=never"]
            providers = ([root / "tests/fixtures/stdlib_adoption_allocation_counts.cpp"]
                         if fixture == "json_object_iteration" else [])
            if not providers:
                command.append("--keep-cpp")
            result = build_fixture(command, binary, native_sources=providers,
                                   native_arguments=native, standard=flags[1],
                                   optimization=flags[2].removeprefix("-"),
                                   env=environment, capture_output=True,
                                   text=True, timeout=180)
            assert result.returncode == 0, (fixture, label, result.stdout, result.stderr)
            assert "// GTI verified-MIR body:" in binary.with_suffix(".gti.cpp").read_text()
            return fixture, label, binary

        def observe(case):
            fixture, label, binary = case
            for count in (range(7) if fixture.endswith("failure") else (0,)):
                result = subprocess.run([str(binary), *("argument" for _ in range(count))],
                                        env=environment, capture_output=True,
                                        text=True, timeout=30)
                if fixture.endswith("failure"):
                    assert result.returncode == 70 and result.stdout == "20\n10\n", (fixture, label, count, result)
                    assert "runtime error[GTI-R0007]" in result.stderr, (fixture, label, count, result.stderr)
                    assert "AddressSanitizer" not in result.stderr and "UndefinedBehaviorSanitizer" not in result.stderr
                else:
                    assert (result.returncode, result.stdout, result.stderr) == (0, "", ""), (fixture, label, result)

        cases = [(f"{fixture}/{label}", (fixture, label, flags, native))
                 for fixture in fixtures for label, flags, native in modes]
        run_cases(cases, build, options.jobs, observe)
        for name, source in negative_sources().items():
            path = output / f"{name}.gti"
            path.write_text(source)
            result = subprocess.run([str(compiler), str(path), "--emit-mir",
                                     "--progress=never", "-o", str(output / "invalid.mir")],
                                    env=environment, capture_output=True,
                                    text=True, timeout=60)
            assert result.returncode == 65 and "error[GTI-S" in result.stderr, (name, result)
            assert "GTI-B0001" not in result.stderr, (name, result.stderr)
        print(f"JSON object iteration: {len(negative_sources())} lifetime/access rejections passed", flush=True)


if __name__ == "__main__":
    main()
