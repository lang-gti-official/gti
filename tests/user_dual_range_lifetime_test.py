#!/usr/bin/env python3
"""General two-owner inline proxies retain physical range/cursor shells."""

import argparse
import os
import pathlib
import shutil
import subprocess
import tempfile

from case_jobs import configured_jobs, positive_jobs, run_cases
from contiguous_iterator_completion_test import MODES


def events(*identities):
    return "".join(f"E{identity}\n" for identity in identities)


def negative_sources(support):
    headers = f'#include "{support.name}"\n#include <std/vector>\n#include <std/string>\n#include <std/panic>\n'
    setup = """mut std::vector<UserDualElement> first = {UserDualElement(1, std::string("left"))};
mut std::vector<UserDualElement> second = {UserDualElement(2, std::string("right"))};
mut auto range = UserDualRange(first.as_span(), second.as_span());
"""
    actions = {
        "first-mutation": "first.clear(); return entry.right().value();",
        "second-mutation": "second.clear(); return entry.left().value();",
        "first-move": "auto stolen = std::move(first); return entry.right().value();",
        "second-move": "auto stolen = std::move(second); return entry.left().value();",
        "physical-range-move": "auto stolen = std::move(range); return entry.left().value();",
        "readonly-element": "entry.left().text().clear(); return entry.right().value();",
        "element-copy": "UserDualElement copied = entry.left(); return copied.value();",
    }
    sources = {
        name: headers + "int main() {\n" + setup + "for (auto& entry : range) { " + action + " } return 0; }\n"
        for name, action in actions.items()
    }
    sources["element-escape"] = headers + """
UserDualProxy& escaped(UserDualRange& range) {
  for (auto& entry : range) { return entry; }
  std::panic("empty user range");
}
int main() {
""" + setup + "return escaped(range).left().value(); }\n"
    sources["proxy-child-movement"] = headers + "int main() {\n" + setup + """
mut auto cursor = range.begin();
UserDualProxy& retained = *cursor;
++cursor;
return retained.left().value();
}
"""
    return sources


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("compiler", type=pathlib.Path)
    parser.add_argument("--fixture", action="append", choices=("user_dual_range_direct", "user_dual_range_minimal", "user_dual_range_completion", "user_single_owner_inline_proxy", "user_external_borrowed_element", "user_dual_range_scalar_siblings", "user_scalar_default_move"))
    parser.add_argument("--mode", action="append", choices=[mode[0] for mode in MODES])
    parser.add_argument("--jobs", type=positive_jobs, default=configured_jobs())
    parser.add_argument("--negative-only", action="store_true")
    parser.add_argument("--no-negative", action="store_true")
    options = parser.parse_args()
    assert not (options.negative_only and options.no_negative)
    compiler = options.compiler.resolve()
    repository = pathlib.Path(__file__).resolve().parent.parent
    fixtures = repository / "tests/fixtures"
    environment = dict(os.environ, GTI_STDLIB_PATH=str(repository / "stdlib"),
                       ASAN_OPTIONS="halt_on_error=1", UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1")
    modes = [mode for mode in MODES if not options.mode or mode[0] in options.mode]
    selected = options.fixture or ("user_dual_range_direct", "user_dual_range_minimal", "user_dual_range_completion", "user_single_owner_inline_proxy", "user_external_borrowed_element", "user_dual_range_scalar_siblings", "user_scalar_default_move")
    with tempfile.TemporaryDirectory(prefix="gti-user-dual-range-") as directory:
        output = pathlib.Path(directory)

        def build(case):
            fixture, label, flags, native = case
            executable = output / f"{fixture}-{label}"
            result = subprocess.run(
                [str(compiler), str(fixtures / f"{fixture}.gti"), *flags,
                 "--progress=never", "--keep-cpp", "-o", str(executable), "--",
                 *native, str(fixtures / "user_dual_range_events.cpp")],
                env=environment, capture_output=True, text=True, timeout=180)
            assert result.returncode == 0, (fixture, label, result.stdout, result.stderr)
            assert "// GTI verified-MIR body:" in executable.with_suffix(".gti.cpp").read_text()
            return fixture, label, executable

        def observe(case):
            fixture, label, executable = case
            result = subprocess.run([str(executable)], env=environment, capture_output=True, text=True, timeout=30)
            expected = {
                "user_dual_range_direct": events(5, 102, 101),
                "user_dual_range_minimal": events(102, 101),
                "user_dual_range_completion": events(12, 12, 11, 12, 12, 11, 10, 21, 22, 21, 20, 104, 102, 103, 101),
                "user_single_owner_inline_proxy": events(100, 100, 100, 200, 200),
                "user_external_borrowed_element": "",
                "user_dual_range_scalar_siblings": "",
                "user_scalar_default_move": "",
            }[fixture]
            assert (result.returncode, result.stdout, result.stderr) == (0, expected, ""), (fixture, label, result)
            if fixture in ("user_dual_range_direct", "user_dual_range_completion"):
                for count, identity, message in (
                    (1, 30, "user dual proxy constructor failure"),
                    (2, 40, "user dual cursor constructor body failure"),
                    (3, 6, "user dual loop body failure"),
                ):
                    result = subprocess.run([str(executable), *("argument" for _ in range(count))],
                                            env=environment, capture_output=True, text=True, timeout=30)
                    assert result.returncode == 70, (fixture, label, count, result)
                    cleanup = (102, 101) if fixture == "user_dual_range_direct" else (104, 102, 103, 101)
                    assert result.stdout == events(identity, 5, *cleanup), (fixture, label, count, result)
                    assert "runtime error[GTI-R0017]" in result.stderr and message in result.stderr, (fixture, label, count, result)
                    assert "GTI-R0014" not in result.stderr, (fixture, label, count, result)

        if not options.negative_only:
            plan = [(f"{fixture}/{label}", (fixture, label, flags, native))
                    for label, flags, native in modes for fixture in selected]
            run_cases(plan, build, options.jobs, observe)
        if not options.no_negative:
            negatives = negative_sources(fixtures / "user_dual_range_support.gti")
            shutil.copyfile(fixtures / "user_dual_range_support.gti", output / "user_dual_range_support.gti")
            for name, source in negatives.items():
                path = output / f"{name}.gti"
                path.write_text(source)
                result = subprocess.run([str(compiler), str(path), "--emit-mir", "--progress=never",
                                         "-o", str(output / "invalid.mir")],
                                        env=environment, capture_output=True, text=True, timeout=60)
                assert result.returncode == 65 and "error[GTI-S" in result.stderr, (name, result)
                assert "GTI-B0001" not in result.stderr, (name, result.stderr)
            print(f"Unrelated two-owner range: {len(negatives)} lifetime/access rejections passed", flush=True)


if __name__ == "__main__":
    main()
