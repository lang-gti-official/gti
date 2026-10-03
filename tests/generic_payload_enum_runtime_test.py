#!/usr/bin/env python3
"""Concrete generic owning sums preserve exact transport and failure cleanup."""

import argparse
import pathlib
import subprocess
import sys
import tempfile


def run(command):
    return subprocess.run(command, text=True, capture_output=True, check=False)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("compiler", type=pathlib.Path)
    parser.add_argument("--quick", action="store_true")
    args = parser.parse_args()
    compiler = args.compiler.resolve()
    source = (pathlib.Path(__file__).parent / "fixtures" /
              "generic_payload_enum.gti").read_text()
    prefix = source.split("void exercise()", 1)[0]
    cases = {
        "success": (source, 0, "1\n4\n3\n2\n"),
        "generic-owner-capture": (prefix + """
int main() {
  {
    Envelope<Event> packet = Envelope<Event>(Event::message(Resource(5)));
    auto callback = [owned = std::move(packet)]() -> int32_t {
      return id(owned.get());
    };
    if (callback() != 5) { return 1; }
  }
  return drops == 1 ? 0 : 1;
}
""", 0, "5\n"),
        "partial-owner": (prefix + """
class Partial<T> {
  T completed;
  int32_t checked;
public:
  Partial(T value, int32_t divisor)
      : completed(std::move(value)), checked(int32_t(1) / divisor) {}
};
int main() {
  Partial<Event> value = Partial<Event>(Event::message(Resource(7)), 0);
  return 1;
}
""", 70, "7\n"),
        "container-cleanup-failure": (prefix.replace(
            "mut int32_t drops = 0;",
            "mut int32_t drops = 0;\nmut int32_t zero = 0;"
        ).replace(
            "std::println(this.identity);",
            "std::println(this.identity);\n"
            "    if (this.identity == 2) { int32_t checked = int32_t(1) / zero; }"
        ) + """
int main() {
  Resource caller = Resource(9);
  mut std::vector<Event> events{};
  events.push_back(Event::message(Resource(1)));
  events.push_back(Event::message(Resource(2)));
  events.push_back(Event::message(Resource(3)));
  return 0;
}
""", 70, "3\n2\n1\n9\n"),
        "later-argument-failure": (prefix + """
void accept<T>(T first, int32_t second) {}
int main() {
  Resource caller = Resource(9);
  mut int32_t divisor = 0;
  accept(Event::message(Resource(7)), int32_t(1) / divisor);
  return 1;
}
""", 70, "7\n9\n"),
        "later-argument-after-move": (prefix + """
void accept<T>(T first, int32_t second) {}
int main() {
  Resource caller = Resource(9);
  Event event = Event::message(Resource(7));
  mut int32_t divisor = 0;
  accept(std::move(event), int32_t(1) / divisor);
  return 1;
}
""", 70, "7\n9\n"),
        "callee-failure": (prefix + """
void accept<T>(T first, int32_t divisor) {
  Resource local = Resource(6);
  int32_t checked = int32_t(1) / divisor;
}
int main() {
  Resource caller = Resource(9);
  accept(Event::message(Resource(7)), 0);
  return 1;
}
""", 70, "6\n7\n9\n"),
        "constructor-later-argument-failure": (prefix + """
class Sink<T> {
public:
  Sink(T first, int32_t second) {}
};
int main() {
  Resource caller = Resource(9);
  mut int32_t divisor = 0;
  Sink<Event> sink = Sink<Event>(Event::message(Resource(7)), int32_t(1) / divisor);
  return 1;
}
""", 70, "7\n9\n"),
        "non-failing-cleanup-after-move": (prefix.replace(
            "    drops = drops + 1;\n    std::println(this.identity);", ""
        ) + """
void accept<T>(T first, int32_t second) {}
int main() {
  Event event = Event::message(Resource(7));
  mut int32_t divisor = 0;
  accept(std::move(event), int32_t(1) / divisor);
  return 1;
}
""", 70, ""),
    }
    construction_only = (pathlib.Path(__file__).parent / "fixtures" /
                         "construction_only_payload_enum.gti").read_text()
    construction_prefix = construction_only.split("void exercise()", 1)[0]
    def fixture_line(fragment):
        matches = [index for index, line in enumerate(construction_only.splitlines(), 1)
                   if fragment in line]
        if len(matches) != 1:
            raise AssertionError(f"expected one fixture line containing {fragment!r}")
        return matches[0]

    parameter_line = fixture_line(": item(std::move(value)), checked")
    fresh_line = fixture_line(": item(Event::message(Payload(identity))), checked")
    called_line = fixture_line(": item(make_event(identity)), checked")
    helper_line = fixture_line("int32_t helper_checked = int32_t(1) / helper_divisor;")
    destructor_line = fixture_line("int32_t checked = int32_t(1) / cleanup_divisor;")
    exact_stderr = {"construction-only-success": ""}
    cases["construction-only-success"] = (
        construction_only, 0, "50\n60\n70\n4\n3\n5\n2\n1\n9\n")
    for name, statement, output, primary_line in (
        ("parameter-field-failure",
         "Envelope<Event> value = Envelope<Event>(Event::message(Payload(1)), 0);",
         "1\n9\n", parameter_line),
        ("fresh-field-failure", "FreshEnvelope value = FreshEnvelope(2, 0);",
         "2\n9\n", fresh_line),
        ("destructor-failure",
         "cleanup_divisor = 0; FreshEnvelope value = FreshEnvelope(2, 1);",
         "60\n2\n9\n", destructor_line),
        ("secondary-failure",
         "cleanup_divisor = 0; FreshEnvelope value = FreshEnvelope(2, 0);",
         "2\n", fresh_line),
        ("called-field-failure", "CalledEnvelope value = CalledEnvelope(2, 0);",
         "2\n9\n", called_line),
        ("called-destructor-failure",
         "cleanup_divisor = 0; CalledEnvelope value = CalledEnvelope(2, 1);",
         "70\n2\n9\n", destructor_line),
        ("called-secondary-failure",
         "cleanup_divisor = 0; CalledEnvelope value = CalledEnvelope(2, 0);",
         "2\n", called_line),
        ("callee-failure",
         "helper_divisor = 0; CalledEnvelope value = CalledEnvelope(2, 1);",
         "2\n9\n", helper_line),
        ("callee-secondary-failure",
         "helper_divisor = 0; cleanup_divisor = 0; "
         "CalledEnvelope value = CalledEnvelope(2, 1);",
         "2\n", helper_line),
    ):
        case_name = "construction-only-" + name
        file_name = case_name + ".gti"
        if name.endswith("secondary-failure"):
            exact_stderr[case_name] = (
                f"{file_name}:{destructor_line}: runtime error[GTI-R0014]: "
                f"failure_during_cleanup; primary [GTI-R0002] at "
                f"{file_name}:{primary_line}; secondary [GTI-R0002]\n")
        else:
            exact_stderr[case_name] = (
                f"{file_name}:{primary_line}: runtime error[GTI-R0002]: "
                "division_by_zero in integer_division\n")
        cases[case_name] = (construction_prefix + f"""
int main() {{
  Guard caller{{}};
  {statement}
  return 0;
}}
""", 70, output)
    modes = [(standard, optimization, [])
             for standard in ("c++20", "c++23")
             for optimization in ("O0", "O1", "O3")]
    modes += [("c++23", "O0", ["-fno-elide-constructors"]),
              ("c++23", "O1", ["-fsanitize=address,undefined",
                                 "-fno-omit-frame-pointer"])]
    if args.quick:
        modes = [("c++23", "O0", [])]
    with tempfile.TemporaryDirectory(prefix="gti-generic-sum-") as temporary:
        root = pathlib.Path(temporary)
        rejected_cases = {
            "implicit-copy": ("""
int main() {
  Event event = Event::message(Resource(1));
  mut std::vector<Event> events{};
  events.push_back(event);
  return 0;
}
""", "GTI-S2018", "copy"),
            "live-borrow": ("""
int main() {
  mut std::vector<Event> events{};
  events.push_back(Event::message(Resource(1)));
  Event& first = events[std::size_t(0)];
  events.clear();
  return id(first);
}
""", "GTI-S2017", "borrow"),
            "moved-owner": ("""
int main() {
  Event event = Event::message(Resource(1));
  std::optional<Event> held = std::optional<Event>(std::move(event));
  return id(event);
}
""", "GTI-S2018", "moved"),
        }
        for name, (body, code, message) in rejected_cases.items():
            path = root / (name + ".gti")
            path.write_text(prefix + body)
            rejected = run([str(compiler), str(path), "--emit-cpp",
                            "-o", str(root / (name + ".cpp"))])
            if (rejected.returncode == 0 or code not in rejected.stderr or
                    message not in rejected.stderr.lower() or
                    "GTI-B000" in rejected.stderr):
                sys.stderr.write(f"{name}: expected source diagnostic {code}\n" +
                                 rejected.stdout + rejected.stderr)
                return 1
        assignment = root / "construction-only-assignment.gti"
        assignment.write_text(construction_prefix + """
int main() {
  Event source = Event::message(Payload(1));
  mut Event destination = Event::idle;
  destination = std::move(source);
  return 0;
}
""")
        rejected = run([str(compiler), str(assignment), "--emit-cpp",
                        "-o", str(root / "construction-only-assignment.cpp")])
        if (rejected.returncode == 0 or "GTI-S2003" not in rejected.stderr or
                "assign" not in rejected.stderr.lower() or
                "GTI-B000" in rejected.stderr):
            sys.stderr.write("construction-only enum must remain nonassignable\n" +
                             rejected.stdout + rejected.stderr)
            return 1
        for name, (text, expected_exit, expected_output) in cases.items():
            path = root / (name + ".gti")
            path.write_text(text)
            for index, (standard, optimization, flags) in enumerate(modes):
                binary = root / f"{name}-{index}"
                command = [str(compiler), str(path), "--std", standard,
                           "-" + optimization, "-o", str(binary)]
                if flags:
                    command += ["--", *flags]
                compiled = run(command)
                if compiled.returncode:
                    sys.stderr.write(f"{name} {standard} {optimization}: compile failed\n" +
                                     compiled.stdout + compiled.stderr)
                    return 1
                executed = run([str(binary)])
                if (executed.returncode != expected_exit or
                        executed.stdout != expected_output or
                        (name in exact_stderr and
                         executed.stderr != exact_stderr[name]) or
                        (name not in exact_stderr and expected_exit == 70 and
                         "division_by_zero" not in executed.stderr) or
                        "AddressSanitizer" in executed.stderr or
                        "runtime error:" in executed.stderr):
                    sys.stderr.write(f"{name} {standard} {optimization}: "
                                     f"exit={executed.returncode} stdout={executed.stdout!r} "
                                     f"stderr={executed.stderr!r}\n")
                    return 1
    print(f"Generic payload sums: {len(cases) * len(modes)} native outcomes passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
