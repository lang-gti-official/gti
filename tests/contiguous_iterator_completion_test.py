#!/usr/bin/env python3
"""Contiguous cursors preserve view extents, owner loans and allocation policy."""

import argparse
import os
import pathlib
import subprocess
import tempfile

from case_jobs import configured_jobs, positive_jobs, run_cases
from native_fixture_build import build_fixture


MODES = [(f"{standard}-{level}", ["--std", standard, f"-{level}"], [])
         for standard in ("c++20", "c++23") for level in ("O0", "O1", "O3")]
MODES += [("no-elide", ["--std", "c++23", "-O0"], ["-fno-elide-constructors"]),
          ("sanitized", ["--std", "c++23", "-O1"],
           ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"])]
FIXTURES = ("contiguous_iterator_completion", "contiguous_iterator_bounds", "contiguous_iterator_cost", "contiguous_iterator_arrow")


def sanitizer_partitions(source):
    """Instrument each independent scenario without losing any fixture text.

    GCC's sanitizer optimization of the combined positive exceeds its native
    compile budget even with one worker. Keep the combined program for every
    other mode; these three bodies retain the exact assertions and allocation
    windows from the one authoritative fixture. Unexpected fixture structure
    fails closed instead of silently excluding a new scenario.
    """
    opening = "int main() {\n"
    closing = "  return 0;\n}\n"
    assert source.count(opening) == 1, "ambiguous combined positive entry"
    prefix, _, body = source.partition(opening)
    assert body.endswith(closing), "combined positive changed its final return"
    body = body.removesuffix(closing)
    boundaries = ("  mut std::vector<int> values =", "  std::process_resource process{};")
    assert all(body.count(boundary) == 1 for boundary in boundaries), "ambiguous scenario boundary"
    span, resource = (body.index(boundary) for boundary in boundaries)
    assert 0 < span < resource < len(body), "out-of-order scenario boundary"
    regions = (body[:span], body[span:resource], body[resource:])
    assert prefix + opening + "".join(regions) + closing == source
    return {name: prefix + opening + region + closing
            for name, region in zip(("string", "span", "resource-vector"), regions, strict=True)}


def negative_sources():
    headers = """import std.string;
import std.vector;
import std.span;
import std.resource_vector;
import std.resource;
"""
    return {
        "vector-snapshot-invalidation": headers + """int main() {
  mut std::vector<int> values = {1, 2}; auto cursor = values.begin();
  values.clear(); return *cursor;
}""",
        "vector-snapshot-exclusive-growth": headers + """int main() {
  mut std::vector<int> values = {1, 2}; auto cursor = values.begin_mut();
  values.push_back(3); return *cursor;
}""",
        "vector-snapshot-exclusive-copy": headers + """int main() {
  mut std::vector<int> values = {1, 2}; auto cursor = values.rbegin_mut();
  auto copy = cursor; return *copy;
}""",
        "vector-snapshot-owner-transfer": headers + """int main() {
  mut std::vector<int> values = {1, 2}; auto cursor = values.begin();
  auto moved = std::move(values); return *cursor;
}""",
        "vector-snapshot-step-after-clear": headers + """int main() {
  mut std::vector<int> values = {1, 2}; mut auto cursor = values.begin();
  values.clear(); ++cursor; return 0;
}""",
        "vector-snapshot-step-after-transfer": headers + """int main() {
  mut std::vector<int> values = {1, 2}; mut auto cursor = values.begin_mut();
  auto moved = std::move(values); ++cursor; return 0;
}""",
        "resource-snapshot-step-after-clear": headers + """int main() {
  std::process_resource process{}; unsafe {
    auto resource = std::bind_resource(process);
    mut std::resource_vector<int, std::process_resource> values{resource}; values.push_back(1);
    mut auto cursor = values.begin(); values.clear(); ++cursor; return 0;
  }
}""",
        "resource-snapshot-step-after-transfer": headers + """int main() {
  std::process_resource process{}; unsafe {
    auto resource = std::bind_resource(process);
    mut std::resource_vector<int, std::process_resource> values{resource}; values.push_back(1);
    mut auto cursor = values.begin_mut(); auto moved = std::move(values); ++cursor; return 0;
  }
}""",
        "vector-snapshot-child-step": headers + """int main() {
  mut std::vector<int> values = {1, 2}; mut auto cursor = values.begin_mut();
  mut int& child = *cursor; ++cursor; return child;
}""",
        "string-readonly": headers + """int main() {
  std::string text{"abc"}; mut auto cursor = text.begin(); *cursor = 'x'; return 0;
}""",
        "string-invalidation": headers + """int main() {
  mut std::string text{"abc"}; auto cursor = text.begin(); text.clear(); return *cursor == 'a' ? 0 : 1;
}""",
        "mutable-span-copy": headers + """int main() {
  mut std::vector<int> values = {1, 2}; mut auto view = values.as_mutable_span();
  auto cursor = view.begin_mut(); auto other = cursor; return *other;
}""",
        "mutable-span-competing": headers + """int main() {
  mut std::vector<int> values = {1, 2}; mut auto view = values.as_mutable_span();
  auto cursor = view.begin_mut(); view[std::size_t(0)] = 3; return *cursor;
}""",
        "mutable-span-child": headers + """int main() {
  mut std::vector<int> values = {1, 2}; mut auto view = values.as_mutable_span();
  mut auto cursor = view.begin_mut(); mut int& child = *cursor; ++cursor; return child;
}""",
        "string-mutable-competing": headers + """int main() {
  mut std::string text{"abc"}; auto cursor = text.begin_mut(); text.clear(); return *cursor == 'a' ? 0 : 1;
}""",
        "string-reverse-readonly": headers + """int main() {
  std::string text{"abc"}; mut auto cursor = text.rbegin(); *cursor = 'x'; return 0;
}""",
        "reverse-span-child": headers + """int main() {
  mut std::vector<int> values = {1, 2}; mut auto view = values.as_mutable_span();
  mut auto cursor = view.rbegin_mut(); mut int& child = *cursor; ++cursor; return child;
}""",
        "readonly-span-arrow": headers + """struct Cell {
  mut int number = 0;
  void set(int value) mut { this.number = value; }
public:
  Cell() = default;
};
int main() {
  std::vector<Cell> values = {Cell()}; auto view = values.as_span();
  auto cursor = view.rbegin(); cursor->set(3); return 0;
}""",
        "readonly-resource-arrow": headers + """struct Cell {
  mut int number = 0;
  void set(int value) mut { this.number = value; }
public:
  Cell() = default;
};
int main() {
  std::process_resource process{}; unsafe {
    auto resource = std::bind_resource(process);
    mut std::resource_vector<Cell, std::process_resource> values{resource}; values.emplace_back();
    auto cursor = values.begin(); cursor->set(3); return 0;
  }
}""",
        "resource-invalidation": headers + """int main() {
  std::process_resource process{}; unsafe {
    auto resource = std::bind_resource(process);
    mut std::resource_vector<int, std::process_resource> values{resource}; values.push_back(1);
    auto cursor = values.begin(); values.clear(); return *cursor;
  }
}""",
        "resource-mutable-competing": headers + """int main() {
  std::process_resource process{}; unsafe {
    auto resource = std::bind_resource(process);
    mut std::resource_vector<int, std::process_resource> values{resource}; values.push_back(1);
    auto cursor = values.begin_mut(); values.push_back(2); return *cursor;
  }
}""",
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("compiler", type=pathlib.Path)
    parser.add_argument("--fixture", action="append", choices=FIXTURES)
    parser.add_argument("--mode", action="append", choices=[mode[0] for mode in MODES])
    parser.add_argument("--jobs", type=positive_jobs, default=configured_jobs())
    parser.add_argument("--no-negative", action="store_true")
    options = parser.parse_args()
    compiler = options.compiler.resolve()
    repository = pathlib.Path(__file__).resolve().parent.parent
    fixtures = repository / "tests/fixtures"
    environment = dict(os.environ, GTI_STDLIB_PATH=str(repository / "stdlib"),
                       ASAN_OPTIONS="halt_on_error=1", UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1")
    modes = [mode for mode in MODES if not options.mode or mode[0] in options.mode]
    fixtures_to_run = options.fixture or FIXTURES
    with tempfile.TemporaryDirectory(prefix="gti-contiguous-iterator-") as directory:
        output = pathlib.Path(directory)
        sanitizer_sources = {}
        if "contiguous_iterator_completion" in fixtures_to_run and any(label == "sanitized" for label, _, _ in modes):
            source = (fixtures / "contiguous_iterator_completion.gti").read_text()
            for region, contents in sanitizer_partitions(source).items():
                path = output / f"contiguous_iterator_completion-{region}.gti"
                path.write_text(contents)
                sanitizer_sources[region] = path

        def build(case):
            fixture, label, flags, native, source, region = case
            name = f"{fixture}-{region}" if region else fixture
            executable = output / f"{name}-{label}"
            command = [str(compiler), str(source), *flags,
                       "--progress=never"]
            native_sources = []
            if fixture in ("contiguous_iterator_completion", "contiguous_iterator_cost"):
                native_sources = [fixtures / "stdlib_adoption_allocation_counts.cpp"]
            else:
                command.append("--keep-cpp")
            result = build_fixture(
                command, executable, native_sources=native_sources,
                native_arguments=native, standard=flags[1],
                optimization=flags[2].removeprefix("-"), env=environment,
                capture_output=True, text=True, timeout=180)
            assert result.returncode == 0, (fixture, label, result.stdout, result.stderr)
            assert "// GTI verified-MIR body:" in executable.with_suffix(".gti.cpp").read_text()
            return fixture, f"{region}/{label}" if region else label, executable

        def observe(case):
            fixture, label, executable = case
            if fixture != "contiguous_iterator_bounds":
                result = subprocess.run([str(executable)], env=environment, capture_output=True, text=True, timeout=30)
                assert (result.returncode, result.stdout, result.stderr) == (0, "", ""), (fixture, label, result)
            else:
                codes = ("GTI-R0016", "GTI-R0016", "GTI-R0007", "GTI-R0007",
                         "GTI-R0016", "GTI-R0016", "GTI-R0016", "GTI-R0016",
                         "GTI-R0016", "GTI-R0016", "GTI-R0016", "GTI-R0016")
                for count, code in enumerate(codes):
                    result = subprocess.run([str(executable), *("argument" for _ in range(count))],
                                            env=environment, capture_output=True, text=True, timeout=30)
                    assert result.returncode == 70 and result.stdout == "", (fixture, label, count, result)
                    assert f"runtime error[{code}]" in result.stderr, (fixture, label, count, result.stderr)

        plan = []
        for label, flags, native in modes:
            for fixture in fixtures_to_run:
                sources = sanitizer_sources.items() if fixture == "contiguous_iterator_completion" and label == "sanitized" else [(None, fixtures / f"{fixture}.gti")]
                for region, source in sources:
                    name = f"{fixture}-{region}" if region else fixture
                    plan.append((f"{name}/{label}", (fixture, label, flags, native, source, region)))
        run_cases(plan, build, options.jobs, observe)
        if not options.no_negative:
            for name, source in negative_sources().items():
                path = output / f"{name}.gti"
                path.write_text(source)
                result = subprocess.run([str(compiler), str(path), "--emit-mir", "--progress=never",
                                         "-o", str(output / "invalid.mir")],
                                        env=environment, capture_output=True, text=True, timeout=60)
                assert result.returncode == 65 and "error[GTI-S" in result.stderr, (name, result)
                assert "GTI-B0001" not in result.stderr, (name, result.stderr)
            print(f"Contiguous cursors: {len(negative_sources())} lifetime/access rejections passed", flush=True)


if __name__ == "__main__":
    main()
