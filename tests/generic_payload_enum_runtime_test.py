#!/usr/bin/env python3
"""Concrete generic owning sums preserve exact transport and failure cleanup."""

import argparse
import pathlib
import subprocess
import sys
import tempfile

from case_jobs import configured_jobs, positive_jobs, run_cases


def run(command):
    return subprocess.run(command, text=True, capture_output=True, check=False)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("compiler", type=pathlib.Path)
    parser.add_argument("--quick", action="store_true")
    parser.add_argument("--jobs", type=positive_jobs, default=configured_jobs(),
                        help="Concurrent compilations; executions stay ordered. Default: serial outside CTest.")
    args = parser.parse_args()
    compiler = args.compiler.resolve()
    source = (pathlib.Path(__file__).parent / "fixtures" /
              "generic_payload_enum.gti").read_text()
    prefix = source.split("void exercise()", 1)[0]
    cases = {
        "call-binding-publication": (prefix + """
Event produce(int divisor) {
  Resource guard = Resource(9);
  return Event::message(Resource(4 / divisor));
}
int main() {
  {
    auto event = produce(1);
    if (drops != 1 || id(event) != 4) return 1;
  }
  return drops == 2 ? 0 : 2;
}
""", 0, "9\n4\n"),
        "call-binding-publication-failure": (prefix + """
Event produce(int divisor) {
  Resource guard = Resource(9);
  return Event::message(Resource(4 / divisor));
}
int main() {
  auto event = produce(0);
  return id(event);
}
""", 70, "9\n"),
        "consuming-binding-publication": (prefix + """
Event produce() { return Event::message(Resource(4)); }
int main() {
  {
    auto event = produce();
    switch (std::move(event)) {
      case Event::idle: return 1;
      case Event::message(resource): {
        if (resource.id() != 4 || drops != 0) return 2;
        break;
      }
    }
  }
  return drops == 1 ? 0 : 3;
}
""", 0, "4\n"),
        "consuming-binding-publication-failure": (prefix + """
Event produce() { return Event::message(Resource(4)); }
int consume(int divisor) {
  auto event = produce();
  switch (std::move(event)) {
    case Event::idle: return 1;
    case Event::message(resource): return resource.id() / divisor;
  }
}
int main() { return consume(0); }
""", 70, "4\n"),
        "observed-owned-field": ("""
#include <std/string>
enum Observed { ready(int& owner), failed(std::string error) };
[[borrowed_from(owner)]] Observed produce(int& owner) {
  if (owner == 3) return Observed::failed(std::string("bad"));
  return Observed::ready(owner);
}
int inspect(Observed& result) {
  switch (result) {
    case Observed::ready(value): return value;
    case Observed::failed(auto& error): return int(error.size());
  }
}
int main() {
  int owner = 3;
  mut auto result = produce(owner);
  if (inspect(result) != 3) return 1;
  switch (result) {
    case Observed::ready(value): return 2;
    case Observed::failed(mut auto& error): {
      error.append("!");
      if (error.size() != std::size_t(4)) return 3;
      break;
    }
  }
  return inspect(result) == 4 ? 0 : 4;
}
""", 0, ""),
        "borrowed-class-payload": ("""
class Carrier {
  int& owner;
public:
  Carrier(int& value) : owner(value) {}
  int read() { return this.owner; }
};
enum Holder { none, item(Carrier value) };
int inspect(Holder& held) {
  switch (held) {
    case Holder::none: return 0;
    case Holder::item(auto& value): return value.read();
  }
}
int main() {
  mut int owner = 17;
  {
    Carrier value = Carrier(owner);
    Holder held = Holder::item(value);
    if (inspect(held) != 17) return 1;
  }
  owner = 23;
  return owner == 23 ? 0 : 2;
}
""", 0, ""),
        "nested-reference-construction": ("""
enum Shared { empty, value(int& owner) };
enum SharedOuter { empty, wrapped(Shared inner) };
enum Exclusive { empty, value(mut int& owner) };
enum ExclusiveOuter { empty, wrapped(Exclusive inner) };
int main() {
  mut int owner = 3;
  {
    auto shared = SharedOuter::wrapped(Shared::value(owner));
  }
  {
    auto exclusive = ExclusiveOuter::wrapped(Exclusive::value(owner));
  }
  owner = 19;
  return owner == 19 ? 0 : 1;
}
""", 0, ""),
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
        assignment.write_text("""
class Borrowed {
  int& owner;
public:
  Borrowed(int& owner) : owner(owner) {}
};
int main() {
  int owner = 1;
  Borrowed source = Borrowed(owner);
  mut Borrowed destination = Borrowed(owner);
  destination = std::move(source);
  return 0;
}
""")
        rejected = run([str(compiler), str(assignment), "--emit-cpp",
                        "-o", str(root / "construction-only-assignment.cpp")])
        if (rejected.returncode == 0 or "GTI-S2003" not in rejected.stderr or
                "assign" not in rejected.stderr.lower() or
                "GTI-B000" in rejected.stderr):
            sys.stderr.write("borrow-carrying class must remain nonassignable\n" +
                             rejected.stdout + rejected.stderr)
            return 1
        plan = []
        for name, (text, expected_exit, expected_output) in cases.items():
            path = root / (name + ".gti")
            path.write_text(text)
            for index, (standard, optimization, flags) in enumerate(modes):
                plan.append((f"{name}/{index}-{standard}-{optimization}",
                             (name, index, standard, optimization, flags,
                              expected_exit, expected_output, exact_stderr.get(name))))

        def compile_case(case):
            name, index, standard, optimization, flags, *_ = case
            binary = root / f"{name}-{index}"
            command = [str(compiler), str(root / (name + ".gti")), "--std", standard,
                       "-" + optimization, "-o", str(binary)]
            if flags:
                command += ["--", *flags]
            compiled = run(command)
            if compiled.returncode:
                raise AssertionError(f"{name} {standard} {optimization}: compile failed\n" +
                                     compiled.stdout + compiled.stderr)
            return case, binary

        def execute_case(compiled):
            case, binary = compiled
            name, _, standard, optimization, _, expected_exit, expected_output, stderr = case
            executed = run([str(binary)])
            if (executed.returncode != expected_exit or
                    executed.stdout != expected_output or
                    (stderr is not None and executed.stderr != stderr) or
                    (stderr is None and expected_exit == 70 and
                     "division_by_zero" not in executed.stderr) or
                    "AddressSanitizer" in executed.stderr or
                    "runtime error:" in executed.stderr):
                raise AssertionError(f"{name} {standard} {optimization}: "
                                     f"exit={executed.returncode} stdout={executed.stdout!r} "
                                     f"stderr={executed.stderr!r}\n")

        run_cases(plan, compile_case, args.jobs, execute_case)
    print(f"Generic payload sums: {len(cases) * len(modes)} native outcomes passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
