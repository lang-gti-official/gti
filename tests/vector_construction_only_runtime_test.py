#!/usr/bin/env python3
"""Empty-slot vector transport needs construction, not element assignment."""

import os
import pathlib
import subprocess
import sys
import tempfile


def run(command):
    environment = dict(os.environ)
    environment["ASAN_OPTIONS"] = "halt_on_error=1"
    environment["UBSAN_OPTIONS"] = "halt_on_error=1:print_stacktrace=1"
    return subprocess.run(command, capture_output=True, text=True,
                          timeout=120, env=environment)


def main():
    if len(sys.argv) != 2:
        raise SystemExit("usage: vector_construction_only_runtime_test.py <gti>")
    compiler = pathlib.Path(sys.argv[1]).resolve()
    fixtures = pathlib.Path(__file__).parent / "fixtures"
    failure = "vector_construction_only_failure.gti"
    primary = (f"{failure}:17: runtime error[GTI-R0002]: "
               "division_by_zero in integer_division\n")
    secondary = (f"{failure}:17: runtime error[GTI-R0014]: "
                 f"failure_during_cleanup; primary [GTI-R0002] at "
                 f"{failure}:17; secondary [GTI-R0002]\n")
    default_failure = "vector_default_constructor_failure.gti"
    default_primary = (f"{default_failure}:26: runtime error[GTI-R0002]: "
                       "division_by_zero in integer_division\n")
    default_secondary = (
        f"{default_failure}:31: runtime error[GTI-R0014]: "
        f"failure_during_cleanup; primary [GTI-R0002] at "
        f"{default_failure}:26; secondary [GTI-R0002]\n")
    default_executions = [
        (0, "1\n2\n3\n300\n12\n200\n11\n100\n10\n90\n", ""),
        (70, "1\n2\n11\n100\n10\n90\n", default_primary),
        (70, "1\n2\n11\n100\n", default_secondary),
    ]
    payload_failure = "vector_optional_payload_failure.gti"
    payload_primary = (f"{payload_failure}:13: runtime error[GTI-R0002]: "
                       "division_by_zero in integer_division\n")
    payload_secondary = (
        f"{payload_failure}:13: runtime error[GTI-R0014]: "
        f"failure_during_cleanup; primary [GTI-R0002] at "
        f"{payload_failure}:13; secondary [GTI-R0002]\n")
    nested_failure = "vector_optional_nested_cleanup_failure.gti"
    nested_primary = (f"{nested_failure}:22: runtime error[GTI-R0002]: "
                      "division_by_zero in integer_division\n")
    nested_secondary = (
        f"{nested_failure}:22: runtime error[GTI-R0014]: "
        f"failure_during_cleanup; primary [GTI-R0002] at "
        f"{nested_failure}:22; secondary [GTI-R0002]\n")
    nested_output = "20\n2\n102\n30\n3\n103\n10\n1\n101\n9\n109\n"
    cases = {
        "vector_construction_only.gti": [
            (0, "beta\nalpha\n1\n2\n3\nthree\ntwo\n102\n103\n9\n", ""),
        ],
        failure: [
            (0, "2\n3\n1\n9\n", ""),
            (70, "2\n3\n1\n9\n", primary),
            (70, "2\n3\n", secondary),
        ],
        default_failure: default_executions + default_executions,
        payload_failure: [
            (0, "20\n2\n30\n3\n10\n1\n9\n", ""),
            (70, "20\n2\n30\n3\n10\n1\n9\n", payload_primary),
            (70, "20\n2\n30\n3\n", payload_secondary),
        ],
        nested_failure: [
            (0, nested_output, ""),
            (70, nested_output, nested_primary),
            (70, "20\n2\n102\n30\n3\n", nested_secondary),
        ],
    }
    owner = """
#include <std/optional>
#include <std/string>
#include <std/vector>
struct Envelope {
  std::optional<std::string> payload{};
};
"""
    rejected = [
        ("assignment", owner + """
int main() {
  mut Envelope target{};
  Envelope source{};
  target = std::move(source);
  return 0;
}
""", "Move assignment is unavailable for type 'Envelope'"),
        ("stronger-concept", owner + """
bool requires_assignment<std::movable T>(T& input) { return true; }
int main() { Envelope input{}; return requires_assignment(input) ? 0 : 1; }
""", "std::movable"),
        ("deleted-move", """
#include <std/vector>
class Immovable {
public:
  Immovable() {}
  Immovable(Immovable& other) = delete;
  Immovable(Immovable&& other) = delete;
};
int main() { std::vector<Immovable> values{}; return 0; }
""", "std::move_constructible"),
        ("borrowed-element", """
#include <std/vector>
struct View { int& value; View(int& source) : value(source) {} };
int main() { std::vector<View> values{}; return 0; }
""", "borrowed state"),
        ("live-borrow", owner + """
int main() {
  mut std::vector<Envelope> values{};
  values.push_back(Envelope());
  Envelope& borrowed = values[std::size_t(0)];
  values.reserve(std::size_t(8));
  return borrowed.payload.has_value() ? 1 : 0;
}
""", "while a reference borrowed from it may still be live"),
    ]
    modes = [(f"{standard}-{level}", ["--std", standard, f"-{level}"], [])
             for standard in ("c++20", "c++23") for level in ("O0", "O1", "O3")]
    modes += [("no-elide", ["--std", "c++23", "-O0"], ["-fno-elide-constructors"]),
              ("asan-ubsan", ["--std", "c++23", "-O1"],
               ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"])]
    with tempfile.TemporaryDirectory(prefix="gti-vector-construction-only-") as temporary:
        root = pathlib.Path(temporary)
        for name, source, expected in rejected:
            path = root / f"{name}.gti"
            path.write_text(source)
            result = run([str(compiler), str(path), "--emit-cpp", "-o", str(root / f"{name}.cpp")])
            if result.returncode == 0 or expected not in result.stderr or "GTI-B0001" in result.stderr:
                raise AssertionError(f"{name}: {result.returncode}\n{result.stdout}{result.stderr}")
        for mode, flags, native in modes:
            for fixture, executions in cases.items():
                binary = root / f"{pathlib.Path(fixture).stem}-{mode}"
                command = [str(compiler), str(fixtures / fixture), *flags, "--keep-cpp", "-o", str(binary)]
                if native:
                    command += ["--", *native]
                built = run(command)
                if built.returncode:
                    raise AssertionError(f"{fixture}/{mode} build:\n{built.stdout}{built.stderr}")
                generated = binary.with_suffix(".gti.cpp").read_text()
                if "// GTI verified-MIR body:" not in generated:
                    raise AssertionError("missing MIR execution witness")
                for count, expected in enumerate(executions):
                    result = run([str(binary), *(["fail"] * count)])
                    actual = (result.returncode, result.stdout, result.stderr)
                    if actual != expected:
                        raise AssertionError(f"{fixture}/{mode}/{count}: {actual!r} != {expected!r}")
                print(f"{fixture}: {mode} passed", flush=True)


if __name__ == "__main__":
    main()
