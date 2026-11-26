#!/usr/bin/env python3
"""Ownership and borrow violations are rejected in every context.

Each violation is a pair of statement sequences that differ only in the
order or target of one operation: the valid twin must build and run, and the
invalid sibling must be rejected with a GTI semantic diagnostic
(docs/language/ownership-and-lifetimes.md: Ownership Transfer, References,
Reference places and endpoints, Source-Defined Vector, Owning Text). Both are
placed in each context: directly in `main`, under a branch, in a one-trip
loop, in a switch case, in a lambda body, in a generic function and in a
method. This is the RFC 0013 invalid-sibling oracle: an invalid sibling that
builds is a soundness defect (`ACCEPTED`), whatever the program then does.

A twin that is rejected is reported as `TWIN(...)` and is not a soundness
defect; it is either the harness's mistake or an over-strict rule, and is
read by hand. A sibling rejected only through a non-semantic diagnostic is
`WRONG-REJECTION`.
"""
import concurrent.futures
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import matrix_support  # noqa: E402

PRELUDE = """import std.utility;
import std.vector;
import std.string;
import std.string_view;
import std.format;
class Owner {
public:
  Owner(int32_t input) : value(input) {}
  Owner(Owner& other) = delete;
  Owner(Owner&& other) = default;
  int32_t get() { return this.value; }
  void set(int32_t v) mut { this.value = v; }
  mut int32_t value;
};
int32_t consume(Owner o) { return o.get(); }
int32_t read(Owner& o) { return o.get(); }
void write(mut Owner& o, int32_t v) { o.set(v); }
void write_two(mut Owner& o, Owner& p) { o.set(p.get() + 1); }
std::string make_text(int32_t n) { return std::format("text{}", n); }
"""

# name: (valid twin, invalid sibling). Statements may use `seed` (int32_t, 1).
VIOLATIONS = {
    "use_after_move": (
        "Owner a = Owner(seed);\n  Owner b = std::move(a);\n  sink += read(b);\n",
        "Owner a = Owner(seed);\n  Owner b = std::move(a);\n  sink += read(a);\n"),
    "move_twice": (
        "Owner a = Owner(seed);\n  sink += consume(std::move(a));\n",
        "Owner a = Owner(seed);\n  sink += consume(std::move(a));\n  sink += consume(std::move(a));\n"),
    "move_in_branch_then_use": (
        "Owner a = Owner(seed);\n  if (seed > 5) { sink += consume(std::move(a)); } else { sink += read(a); }\n",
        "Owner a = Owner(seed);\n  if (seed > 5) { sink += consume(std::move(a)); }\n  sink += read(a);\n"),
    "move_in_loop": (
        "Owner a = Owner(seed);\n  for (mut int32_t i = 0; i < 2; i++) { sink += read(a); }\n  sink += consume(std::move(a));\n",
        "Owner a = Owner(seed);\n  for (mut int32_t i = 0; i < 2; i++) { sink += consume(std::move(a)); }\n"),
    "mutate_while_borrowed": (
        "mut Owner a = Owner(seed);\n  {\n    Owner& v = a;\n    sink += v.get();\n  }\n  a.value = 9;\n",
        "mut Owner a = Owner(seed);\n  Owner& v = a;\n  a.value = 9;\n  sink += v.get();\n"),
    "write_through_two_mutable": (
        "mut Owner a = Owner(seed);\n  {\n    mut Owner& x = a;\n    x.value = 1;\n  }\n  {\n    mut Owner& y = a;\n    y.value = 2;\n  }\n  sink += a.get();\n",
        "mut Owner a = Owner(seed);\n  mut Owner& x = a;\n  mut Owner& y = a;\n  x.value = 1;\n  y.value = 2;\n  sink += a.get();\n"),
    "mutable_and_shared_arguments": (
        "mut Owner a = Owner(seed);\n  Owner b = Owner(seed);\n  write_two(a, b);\n  sink += a.get();\n",
        "mut Owner a = Owner(seed);\n  write_two(a, a);\n  sink += a.get();\n"),
    "element_ref_across_push": (
        "mut std::vector<Owner> items{};\n  items.push_back(Owner(seed));\n  items.push_back(Owner(2));\n  Owner& r = items[0];\n  sink += r.get();\n",
        "mut std::vector<Owner> items{};\n  items.push_back(Owner(seed));\n  Owner& r = items[0];\n  items.push_back(Owner(2));\n  sink += r.get();\n"),
    "push_during_range_for": (
        "mut std::vector<Owner> items{};\n  items.push_back(Owner(seed));\n  mut int32_t extra = 0;\n  for (auto& e : items) { extra += e.get(); }\n  items.push_back(Owner(extra));\n  sink += extra;\n",
        "mut std::vector<Owner> items{};\n  items.push_back(Owner(seed));\n  for (auto& e : items) { items.push_back(Owner(e.get())); }\n  sink += 1;\n"),
    "move_from_vector_while_borrowed": (
        "mut std::vector<Owner> items{};\n  items.push_back(Owner(seed));\n  sink += read(items[0]);\n  mut std::vector<Owner> other = std::move(items);\n  sink += int32_t(other.size());\n",
        "mut std::vector<Owner> items{};\n  items.push_back(Owner(seed));\n  Owner& r = items[0];\n  mut std::vector<Owner> other = std::move(items);\n  sink += r.get();\n"),
    "view_of_temporary_text": (
        "std::string text = make_text(seed);\n  std::string_view v = text.view();\n  sink += int32_t(v.size());\n",
        "std::string_view v = make_text(seed).view();\n  sink += int32_t(v.size());\n"),
    "view_outlives_text": (
        "std::string text = make_text(seed);\n  {\n    std::string_view v = text.view();\n    sink += int32_t(v.size());\n  }\n",
        "mut std::string_view v = \"x\";\n  {\n    std::string text = make_text(seed);\n    v = text.view();\n  }\n  sink += int32_t(v.size());\n"),
    "moved_capture_use": (
        "Owner a = Owner(seed);\n  auto f = [owned = std::move(a)]() -> int32_t { return owned.get(); };\n  sink += f();\n",
        "Owner a = Owner(seed);\n  auto f = [owned = std::move(a)]() -> int32_t { return owned.get(); };\n  sink += f() + a.get();\n"),
    "move_borrowed_owner": (
        "Owner a = Owner(seed);\n  sink += read(a);\n  Owner b = std::move(a);\n  sink += b.get();\n",
        "Owner a = Owner(seed);\n  Owner& r = a;\n  Owner b = std::move(a);\n  sink += r.get() + b.get();\n"),
    "assign_while_borrowed": (
        "mut Owner a = Owner(seed);\n  sink += read(a);\n  a = Owner(4);\n  sink += a.get();\n",
        "mut Owner a = Owner(seed);\n  Owner& r = a;\n  a = Owner(4);\n  sink += r.get();\n"),
}

# Each context wraps BODY; `seed` is 1 and `sink` accumulates.
CONTEXTS = {
    "main": ("", "  mut int32_t sink = 0;\n  int32_t seed = 1;\n  BODY"),
    "branch": ("", "  mut int32_t sink = 0;\n  int32_t seed = 1;\n  if (seed > 0) {\n  BODY  }\n"),
    "loop": ("", "  mut int32_t sink = 0;\n  for (mut int32_t seed = 1; seed < 2; seed++) {\n  BODY  }\n"),
    "switch": ("", "  mut int32_t sink = 0;\n  int32_t seed = 1;\n  switch (seed) {\n  case 1: {\n  BODY    break;\n  }\n  default: break;\n  }\n"),
    "lambda": ("", "  mut int32_t sink = 0;\n  auto body = [](int32_t seed) -> int32_t {\n  mut int32_t sink = 0;\n  BODY  return sink;\n  };\n  sink += body(1);\n"),
    "generic": ("int32_t run<T>(T unused) {\n  int32_t seed = 1;\n  mut int32_t sink = 0;\n  BODY  return sink;\n}\n",
                "  mut int32_t sink = 0;\n  sink += run(1);\n"),
    "method": ("class Host {\npublic:\n  Host() {}\n  int32_t run(int32_t seed) {\n  mut int32_t sink = 0;\n  BODY  return sink;\n  }\n};\n",
               "  mut int32_t sink = 0;\n  Host host = Host();\n  sink += host.run(1);\n"),
}


def program(context, body):
    declarations, main = CONTEXTS[context]
    return (PRELUDE + declarations.replace("BODY", body) + "int main() {\n"
            + main.replace("BODY", body)
            + "  if (sink < 0) { return 1; }\n  std::println(\"ok\");\n  return 0;\n}\n")


def options(parser):
    parser.add_argument("--violation", nargs="*", choices=sorted(VIOLATIONS),
                        default=sorted(VIOLATIONS))
    parser.add_argument("--context", nargs="*", choices=sorted(CONTEXTS),
                        default=sorted(CONTEXTS))
    parser.add_argument("--jobs", type=int, default=4)


def main():
    matrix = matrix_support.Matrix(__doc__.split("\n\n")[0], "invalid-sibling",
                                   default_levels="O0", extra=options)
    work = []
    for violation in matrix.args.violation:
        valid, invalid = VIOLATIONS[violation]
        for context in matrix.args.context:
            for kind, body in (("twin", valid), ("sibling", invalid)):
                key = f"{violation}/{context}/{kind}"
                path = matrix.out / f"{violation}__{context}__{kind}.gti"
                path.write_text(program(context, body), encoding="utf-8")
                work.append((key, path, kind))

    def classify(item):
        key, path, kind = item
        if kind == "twin":
            return key, kind, matrix.classify(path, key=key, expect_stdout="ok\n")
        return key, kind, matrix.classify(path, key=key, execute=False,
                                          expect_build_error=r"error\[GTI-S")

    failures, twins = [], []
    with concurrent.futures.ThreadPoolExecutor(matrix.args.jobs) as pool:
        for key, kind, verdict in pool.map(classify, work):
            print(f"{key:56} {verdict}", flush=True)
            if kind == "twin" and verdict != "ok" and not matrix_support.hard(verdict):
                twins.append((key, verdict))
            elif matrix_support.hard(verdict) or verdict.startswith("WRONG-REJECTION"):
                failures.append((key, verdict))
    print(f"{len(work)} programs at {','.join(matrix.levels)}: {len(failures)} "
          f"hard failures, {len(twins)} rejected valid twins")
    for key, verdict in failures:
        print(f"  {verdict}  {key}")
    for key, verdict in twins:
        print(f"  TWIN({verdict})  {key}")
    return matrix.finish(failures)


if __name__ == "__main__":
    sys.exit(main())
