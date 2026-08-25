#!/usr/bin/env python3

"""Check that the QA tools still detect what they claim to detect.

A QA tool that stops checking anything reports success, which is the worst
possible failure mode: it converts an unexamined surface into a green tick.
That happened three times while these tools were being written —

  * `lsp_probe` looked for `selectionRange`, the server answers with
    `location.range`, so it skipped every symbol and passed having compared
    nothing;
  * `corpus_differential` folded build failures into one `skipped` count, so
    two fixtures raising `GTI-B0001` were invisible behind a number;
  * a mutation test on `place_shape_matrix` changed the expectation *and* the
    generated source, so it could not fail.

Each was caught by hand. This runs those checks every time instead.

    python3 scripts/debug/selftest.py

The classifiers are driven with synthetic inputs rather than real compiles, so
this is fast and needs no build. It verifies the *decision* logic — that each
tool separates pass from fail — not that the compiler is correct.
"""

from __future__ import annotations

import importlib.util
from pathlib import Path
import sys

DEBUG = Path(__file__).resolve().parent


def load(name: str, filename: str | None = None):
    location = DEBUG / (filename or f"{name}.py")
    spec = importlib.util.spec_from_file_location(name, location)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"cannot load {location}")
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


class Result:
    def __init__(self) -> None:
        self.failures: list[str] = []

    def check(self, label: str, actual, expected) -> None:
        if actual == expected:
            print(f"  {label:<52} ok")
        else:
            self.failures.append(label)
            print(f"  {label:<52} FAILED  got {actual!r}, want {expected!r}")


def check_corpus_differential(result: Result) -> None:
    """Its verdicts must separate divergence, agreement, and noise."""
    module = load("corpus_differential")
    args = (Path("gti"), "c++", Path("/tmp"), Path("x.gti"), ["O0", "O3"], 10)

    def observations(**by_level):
        def fake(g, c, s, o, level, t):
            return by_level[level]
        return fake

    ran = lambda status, out="": module.Observation("ran", status, out, "")

    module.build_and_run = observations(O0=ran(3), O3=ran(9))
    result.check("corpus: differing exit is DIVERGENT",
                 module.compare_program(*args)[0], "DIVERGENT")

    module.build_and_run = observations(O0=ran(3), O3=ran(3))
    result.check("corpus: agreeing exit is ok",
                 module.compare_program(*args)[0], "ok")

    module.build_and_run = observations(O0=ran(3, "a"), O3=ran(3, "b"))
    result.check("corpus: differing stdout is DIVERGENT",
                 module.compare_program(*args)[0], "DIVERGENT")

    counter = {"n": 0}

    def unstable(g, c, s, o, level, t):
        counter["n"] += 1
        return ran(counter["n"])

    module.build_and_run = unstable
    result.check("corpus: self-disagreement is nondet",
                 module.compare_program(*args)[0], "nondet")

    # An internal error must not hide inside the skipped count — #112.
    module.build_and_run = lambda g, c, s, o, level, t: module.Observation(
        "build-failed", None, "", f"{level}: GTI-B0001 internal")
    result.check("corpus: internal error is ICE, not skipped",
                 module.compare_program(*args)[0], "ICE")

    module.build_and_run = lambda g, c, s, o, level, t: module.Observation(
        "build-failed", None, "", f"{level}: error[GTI-S2003] ordinary")
    result.check("corpus: ordinary rejection is skipped",
                 module.compare_program(*args)[0], "skipped")


def check_lsp_probe(result: Result) -> None:
    """It must read both reply shapes, and slice UTF-16 correctly."""
    module = load("lsp_probe")

    flat = [{"name": "alpha",
             "location": {"range": {"start": {"line": 0, "character": 1},
                                    "end": {"line": 0, "character": 6}}}}]
    result.check("lsp: reads flat SymbolInformation",
                 [e["name"] for e in module.symbol_entries(flat)], ["alpha"])

    nested = [{"name": "outer",
               "range": {"start": {"line": 0, "character": 0},
                         "end": {"line": 0, "character": 9}},
               "children": [{"name": "inner",
                             "selectionRange": {
                                 "start": {"line": 1, "character": 0},
                                 "end": {"line": 1, "character": 5}}}]}]
    result.check("lsp: reads hierarchical DocumentSymbol",
                 [e["name"] for e in module.symbol_entries(nested)],
                 ["outer", "inner"])

    result.check("lsp: empty result yields no entries",
                 module.symbol_entries([]), [])

    # A surrogate pair is two UTF-16 units; slicing must not treat it as one.
    line = "🎺x"
    result.check("lsp: slices past a surrogate pair",
                 module.slice_utf16(line, 2, 3), "x")


def check_explain_rejection(result: Result) -> None:
    """Probe detection must find a lambda introducer several blocks out."""
    module = load("explain_rejection")
    lines = [
        "void f() {",
        "  const auto probe = [&]() {",
        "    if (condition) {",
        "      return false;",
        "    }",
        "  };",
        "  return false;",
        "}",
    ]
    result.check("explain: rejection inside a lambda is a probe",
                 module.guarding_comment(lines, 4)[2], True)
    result.check("explain: rejection in the body proper is not",
                 module.guarding_comment(lines, 7)[2], False)


def check_wrong_code_header(result: Result) -> None:
    """The expected-status header is what makes a wrong-code case checkable."""
    location = (DEBUG.parent.parent / "tests" / "known_failures" /
                "wrong-code" / "check.py")
    spec = importlib.util.spec_from_file_location("wrong_code_check", location)
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)

    import tempfile
    directory = Path(tempfile.mkdtemp(prefix="gti-selftest-"))
    with_header = directory / "with.gti"
    with_header.write_text("// expected exit status: 7\nint main() { return 7; }\n")
    without = directory / "without.gti"
    without.write_text("int main() { return 7; }\n")

    result.check("wrong-code: reads a declared status",
                 module.expected_status(with_header), 7)
    result.check("wrong-code: missing header is None, not a default",
                 module.expected_status(without), None)


def main() -> int:
    result = Result()
    print("corpus_differential:")
    check_corpus_differential(result)
    print("lsp_probe:")
    check_lsp_probe(result)
    print("explain_rejection:")
    check_explain_rejection(result)
    print("wrong-code/check.py:")
    check_wrong_code_header(result)

    print()
    if result.failures:
        print(f"{len(result.failures)} self-test(s) failed — a QA tool is no "
              "longer detecting what it claims to")
        return 1
    print("every classifier separates pass from fail")
    return 0


if __name__ == "__main__":
    sys.exit(main())
