#!/usr/bin/env python3

"""Census the MIR shapes the C++ backend has no text vocabulary for.

`tests/mir_census_test.py` counts bodies per example: it guards the shipped
corpus against drift. It cannot answer the different question this driver
asks — *which valid MIR shapes can the backend not emit at all?*

The backend admits shapes through many individually proved fail-closed routes.
Each route proves one shape; nothing measures their union, so a shape outside
every route is discovered only when somebody writes a program that produces
it and gets `GTI-B0001`. That is a poor way to find them: gti#207, gti#208,
gti#210 and gti#211 were all found by accident, and all four are the same
defect class.

This driver turns that discovery into a measurement. For every program that
declines it reads the body the diagnostic names, extracts a normalized shape
signature from the verified MIR, and aggregates signatures across the whole
sweep. The output is a ranked list of missing vocabularies with a
representative program for each — a worklist rather than an anecdote.

    python3 scripts/debug/decline_census.py --compiler build/gti
    python3 scripts/debug/decline_census.py --compiler build/gti --corpus examples
    python3 scripts/debug/decline_census.py --compiler build/gti --update

Two facts make the census possible without instrumenting the compiler: a
declining program still emits verified MIR (only the backend declined), and
the diagnostic names the exact body (`body-kind` and `owner`). Gate-level
attribution for one reproducer remains `explain_rejection.py`'s job; this
driver answers the population question instead.

Outcomes are separated so the driver's own defects stay visible. `decline` is
a missing backend vocabulary. `rejected` means an ordinary diagnostic, which
for a generated program means *this generator* emitted invalid GTI and must be
fixed rather than counted. `invalid-mir` is an internal error before the
backend, which is a different defect class and is reported apart from the
vocabulary census.
"""

from __future__ import annotations

import argparse
import collections
import dataclasses
import json
import pathlib
import re
import subprocess
import tempfile

TIMEOUT_SECONDS = 240

# MIR text prints these as integers; the census renders names so a signature
# survives an enum reordering being reviewed rather than silently changing.
PLACE_ROOTS = {
    0: "Binding",
    1: "Symbol",
    2: "This",
    3: "Temporary",
    4: "Value",
    5: "Loan",
}

LIFECYCLE_KINDS = {
    0: "Initialize",
    1: "Move",
    2: "Reparent",
    3: "Replace",
    4: "TransferOut",
    5: "Drop",
}

BODY_KINDS = {
    0: "Module",
    1: "FieldInitializers",
    2: "StaticFieldInitializers",
    3: "Function",
    4: "Constructor",
    5: "Destructor",
    6: "Lambda",
    7: "HostedStartup",
}

# Instructions whose shape decides emission vocabulary. Compute/Load traffic is
# spelled by every route and only adds noise to a signature.
SIGNIFICANT_INSTRUCTIONS = {
    "assign", "construct", "initialize", "move", "drop", "call", "modify",
}

DECLINE_PATTERN = re.compile(
    r"outside the complete C\+\+ text vocabulary for "
    r"'(?P<symbol>[^']*)' \(body-kind (?P<kind>\d+) owner (?P<owner>\d+)\)")


@dataclasses.dataclass
class Program:
    """One census input: a name, the source to compile, and its origin.

    Origin decides how an ordinary diagnostic reads. A generated program is
    written to be valid, so a diagnostic means *this driver* is wrong. A
    corpus program may legitimately not compile standalone — a module unit
    that is not an entry point, or a deliberately invalid negative example —
    so its diagnostic is reported as "not a standalone program" instead.
    """

    name: str
    source: str
    origin: str = "generated"


@dataclasses.dataclass
class Outcome:
    program: str
    kind: str                      # ok | decline | rejected | invalid-mir
    origin: str = "generated"
    detail: str = ""
    features: set[str] = dataclasses.field(default_factory=set)
    body_kind: str = ""
    # Derived after the sweep: the features no emitted body explains.
    signature: tuple[str, ...] = ()


def generated_programs() -> list[Program]:
    """The built-in shape space: value kind x producer x destination.

    Every program is written to be valid GTI. A `rejected` outcome therefore
    means this generator is wrong, which the report says explicitly rather
    than folding into the census.
    """
    values = [
        ("cleanup-free class",
         "class cell {\n"
         "public:\n"
         "  cell(int32_t v) : value(v) {}\n"
         "  int32_t get() { return value; }\n"
         "  mut int32_t value = 0;\n"
         "};\n",
         "cell", "cell({seed})"),
        ("cleanup-owning class",
         "class tracked {\n"
         "public:\n"
         "  tracked(int32_t v) : value(v) {}\n"
         "  ~tracked() {}\n"
         "  int32_t get() { return value; }\n"
         "  mut int32_t value = 0;\n"
         "};\n",
         "tracked", "tracked({seed})"),
    ]

    # (name, statements building `source_value` of type T carrying 7)
    producers = [
        ("constructor prvalue", None),
        ("call result",
         "  {type} produced = make_{type}();\n"),
        ("moved local",
         "  mut {type} staged = {make7};\n"),
    ]

    # (name, statements consuming the produced value)
    destinations = [
        ("new binding", "  {type} sink = {value};\n"
                        "  int32_t observed = sink.get();\n"),
        ("existing binding", "  mut {type} sink = {make0};\n"
                             "  sink = {value};\n"
                             "  int32_t observed = sink.get();\n"),
        ("array element", "  mut {type} sink[2] = {{{make0}, {make0}}};\n"
                          "  sink[0] = {value};\n"
                          "  int32_t observed = sink[0].get();\n"),
        ("class field", "  mut holder sink = holder();\n"
                        "  sink.held = {value};\n"
                        "  int32_t observed = sink.held.get();\n"),
        ("vector element", "  mut std::vector<{type}> sink = "
                           "std::vector<{type}>();\n"
                           "  sink.push_back({make0});\n"
                           "  sink[std::size_t(0)] = {value};\n"
                           "  int32_t observed = "
                           "sink[std::size_t(0)].get();\n"),
        ("by-value argument", "  int32_t observed = consume({value});\n"),
    ]

    programs: list[Program] = []
    for value_name, declaration, type_name, make in values:
        for producer_name, producer_statements in producers:
            for destination_name, destination_statements in destinations:
                if producer_name == "moved local":
                    value_expression = "std::move(staged)"
                elif producer_name == "call result":
                    value_expression = "std::move(produced)"
                else:
                    value_expression = make.format(seed=7)
                body = ""
                if producer_statements is not None:
                    body += producer_statements.format(
                        type=type_name, make7=make.format(seed=7))
                body += destination_statements.format(
                    type=type_name, value=value_expression,
                    make0=make.format(seed=0))

                support = ""
                if "make_" in body:
                    support += (f"{type_name} make_{type_name}() {{\n"
                                f"  return {make.format(seed=7)};\n"
                                f"}}\n\n")
                if "holder sink" in body:
                    support += (f"class holder {{\n"
                                f"public:\n"
                                f"  holder() : held({make.format(seed=0)}) {{}}\n"
                                f"  mut {type_name} held;\n"
                                f"}};\n\n")
                if "consume(" in body:
                    support += (f"int32_t consume({type_name} taken) {{\n"
                                f"  return taken.get();\n"
                                f"}}\n\n")

                source = ("#include <std/vector>\n"
                          "#include <std/utility>\n\n"
                          f"{declaration}\n{support}"
                          "int main() {\n"
                          f"{body}"
                          "  return observed == 7 ? 0 : 1;\n"
                          "}\n")
                name = (f"{value_name} / {producer_name} / "
                        f"{destination_name}")
                programs.append(Program(name=name, source=source,
                                        origin="generated"))
    return programs


def corpus_programs(roots: list[pathlib.Path]) -> list[Program]:
    programs: list[Program] = []
    for root in roots:
        for path in sorted(root.rglob("*.gti")):
            programs.append(Program(name=str(path),
                                    source=path.read_text(encoding="utf-8"),
                                    origin="corpus"))
    return programs


def body_segment(mir: list[str], kind: int, owner: int) -> list[str]:
    """The MIR lines of exactly the body the decline diagnostic named."""
    header = re.compile(rf"^body kind={kind} owner={owner}\b")
    boundary = re.compile(r"^(body|function|class|hosted|enum|global)")
    start = None
    for index, line in enumerate(mir):
        if header.match(line):
            start = index
            break
    if start is None:
        return []
    for index in range(start + 1, len(mir)):
        if boundary.match(mir[index]):
            return mir[start:index]
    return mir[start:]


def body_features(segment: list[str]) -> set[str]:
    """The facts about one body that decide which emission route may claim it.

    Each feature keeps an instruction's kind, the root and projection shape of
    its destination place, whether it stages a result, and the lifecycle
    transitions it carries. Identifiers, types and ordering are dropped, so
    the same vocabulary aggregates across different programs.
    """
    places: dict[str, tuple[str, bool]] = {}
    for line in segment:
        match = re.match(r"^\s*(p\d+) .*?root=(\d+).*?projections=\[([^\]]*)\]",
                         line)
        if match:
            places[match.group(1)] = (
                PLACE_ROOTS.get(int(match.group(2)), match.group(2)),
                bool(match.group(3).strip()))

    features: set[str] = set()
    for line in segment:
        match = re.match(r"^\s*i\d+ ([a-z-]+) ", line)
        if match is None or match.group(1) not in SIGNIFICANT_INSTRUCTIONS:
            continue
        instruction = match.group(1)
        feature = instruction
        destination = re.search(r" destination=(\S+)", line)
        if destination and destination.group(1) != "-":
            root, projected = places.get("p" + destination.group(1),
                                         ("?", False))
            feature += f":dest={root}"
            if projected:
                feature += "+proj"
        if re.search(r" success-result-drop=(?!-)\S+", line):
            feature += ":staged"
        lifecycle = re.search(r"lifecycle=\[([^\]]*)\]", line)
        if lifecycle and lifecycle.group(1).strip():
            kinds = sorted({
                LIFECYCLE_KINDS.get(int(kind), kind)
                for kind in re.findall(r"kind=(\d+)", lifecycle.group(1))})
            feature += ":" + "+".join(kinds)
        features.add(feature)
    return features


def classify(compiler: pathlib.Path, cxx: str, program: Program,
             workdir: pathlib.Path, index: int) -> Outcome:
    source = workdir / f"case{index}.gti"
    source.write_text(program.source, encoding="utf-8")
    compiled = subprocess.run(
        [str(compiler), str(source), "-o", str(workdir / f"case{index}"),
         "--cxx", cxx],
        capture_output=True, text=True, timeout=TIMEOUT_SECONDS, check=False)
    if compiled.returncode == 0:
        # Every body of a program that emitted is, by construction, spellable.
        # Those features are the evidence separating a declining body's
        # distinctive shape from its ordinary surroundings.
        spellable: set[str] = set()
        emitted_path = workdir / f"case{index}.ok.mir"
        emitted_ok = subprocess.run(
            [str(compiler), str(source), "--emit-mir", "-o",
             str(emitted_path)],
            capture_output=True, text=True, timeout=TIMEOUT_SECONDS,
            check=False)
        if emitted_ok.returncode == 0 and emitted_path.is_file():
            spellable = body_features(
                emitted_path.read_text(encoding="utf-8").splitlines())
        return Outcome(program=program.name, origin=program.origin,
                       kind="ok", features=spellable)
    combined = compiled.stdout + compiled.stderr
    decline = DECLINE_PATTERN.search(combined)
    if decline is None:
        if "GTI-B0001" in combined:
            reason = "failed to construct valid MIR" \
                if "failed to construct valid MIR" in combined else "internal"
            return Outcome(program=program.name, origin=program.origin,
                           kind="invalid-mir", detail=reason)
        code = re.search(r"error\[(GTI-[A-Z0-9]+)\]", combined)
        return Outcome(program=program.name, origin=program.origin,
                       kind="rejected",
                       detail=code.group(1) if code else "no-code")

    mir_path = workdir / f"case{index}.mir"
    emitted = subprocess.run(
        [str(compiler), str(source), "--emit-mir", "-o", str(mir_path)],
        capture_output=True, text=True, timeout=TIMEOUT_SECONDS, check=False)
    body_kind = BODY_KINDS.get(int(decline.group("kind")),
                               decline.group("kind"))
    if emitted.returncode != 0 or not mir_path.is_file():
        return Outcome(program=program.name, origin=program.origin,
                       kind="decline", detail="MIR unavailable",
                       body_kind=body_kind)
    segment = body_segment(mir_path.read_text(encoding="utf-8").splitlines(),
                           int(decline.group("kind")),
                           int(decline.group("owner")))
    if not segment:
        return Outcome(program=program.name, origin=program.origin,
                       kind="decline", detail="body not located",
                       body_kind=body_kind)
    return Outcome(program=program.name, origin=program.origin,
                   kind="decline", features=body_features(segment),
                   body_kind=body_kind)


def derive_signatures(outcomes: list[Outcome], learn_from_corpus: bool
                      ) -> None:
    """Reduce each decline to the features no emitted body explains.

    A feature present in some body the backend *did* emit is spellable, so it
    cannot be what was missing here. Subtracting them leaves the candidate
    vocabulary. When nothing remains the shapes are individually spellable and
    the gap is their combination, which the report says rather than inventing
    a cause.

    The baseline deliberately defaults to the generated programs alone. They
    share one skeleton with exactly one axis varied, so subtracting them
    isolates that axis. Unrelated corpus programs use the same features in
    contexts where they *are* spellable, which washes the distinguishing
    feature out: measured against `examples/`, the same 21 declines went from
    eight sharp vocabularies to fifteen mostly-unexplained ones. Corpus files
    are therefore censused but do not widen the baseline unless asked.
    """
    spellable: set[str] = set()
    for outcome in outcomes:
        if outcome.kind != "ok":
            continue
        if outcome.origin == "corpus" and not learn_from_corpus:
            continue
        spellable |= outcome.features
    for outcome in outcomes:
        if outcome.kind != "decline":
            continue
        distinctive = outcome.features - spellable
        if distinctive:
            outcome.signature = tuple(sorted(distinctive))
            continue
        # Every feature is spellable somewhere, so the gap is the combination.
        # Report the whole shape, marked, rather than an opaque "unknown":
        # the reader still needs to see what the body asks for.
        outcome.signature = ("(every feature is spellable alone; the "
                             "combination is not)",) + tuple(
                                 sorted(outcome.features))


def report(outcomes: list[Outcome], learn_from_corpus: bool = False) -> dict:
    derive_signatures(outcomes, learn_from_corpus)
    declines = [o for o in outcomes if o.kind == "decline"]
    grouped: dict[tuple[str, tuple[str, ...]], list[str]] = \
        collections.defaultdict(list)
    for outcome in declines:
        grouped[(outcome.body_kind, outcome.signature)].append(outcome.program)

    print(f"{len(outcomes)} program(s): "
          f"{sum(1 for o in outcomes if o.kind == 'ok')} ok, "
          f"{len(declines)} declined, "
          f"{sum(1 for o in outcomes if o.kind == 'invalid-mir')} invalid-mir, "
          f"{sum(1 for o in outcomes if o.kind == 'rejected')} rejected")

    generated_rejects = [o for o in outcomes
                         if o.kind == "rejected" and o.origin == "generated"]
    if generated_rejects:
        print("\ngenerator defects — a generated program must be valid GTI, "
              "so these are this driver's bugs, not census data:")
        for outcome in generated_rejects:
            print(f"  {outcome.program}: {outcome.detail}")

    corpus_rejects = [o for o in outcomes
                      if o.kind == "rejected" and o.origin == "corpus"]
    if corpus_rejects:
        print(f"\n{len(corpus_rejects)} corpus file(s) are not standalone "
              "programs (a non-entry module unit or a deliberately invalid "
              "example); they contribute nothing either way:")
        for outcome in corpus_rejects[:5]:
            print(f"  {outcome.program}: {outcome.detail}")
        if len(corpus_rejects) > 5:
            print(f"  ... and {len(corpus_rejects) - 5} more")

    invalid = [o for o in outcomes if o.kind == "invalid-mir"]
    if invalid:
        print("\ninternal errors before the backend (a different defect "
              "class):")
        for outcome in invalid:
            print(f"  {outcome.program}: {outcome.detail}")

    if grouped:
        print(f"\n{len(grouped)} missing backend vocabular"
              f"{'y' if len(grouped) == 1 else 'ies'}, most frequent first:")
        ordered = sorted(grouped.items(), key=lambda item: (-len(item[1]),
                                                            item[0]))
        for (body_kind, signature), programs in ordered:
            print(f"\n  [{len(programs)}x] {body_kind} body")
            for feature in signature:
                print(f"      {feature}")
            print(f"      e.g. {programs[0]}")
            if len(programs) > 1:
                print(f"      ... and {len(programs) - 1} more")

    return {
        "totals": {
            kind: sum(1 for o in outcomes if o.kind == kind)
            for kind in ("ok", "decline", "invalid-mir", "rejected")
        },
        "vocabularies": [
            {
                "body_kind": body_kind,
                "signature": list(signature),
                "count": len(programs),
                "example": programs[0],
            }
            for (body_kind, signature), programs in
            sorted(grouped.items(), key=lambda item: (-len(item[1]), item[0]))
        ],
    }


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Census the MIR shapes the backend cannot emit.")
    parser.add_argument("--compiler", default="build/gti")
    parser.add_argument("--cxx", default="c++")
    parser.add_argument("--corpus", action="append", default=[],
                        help="also census every .gti below this directory")
    parser.add_argument("--baseline", type=pathlib.Path,
                        help="compare against (or with --update, write) "
                             "this JSON census")
    parser.add_argument("--update", action="store_true",
                        help="rewrite the baseline instead of comparing")
    parser.add_argument("--learn-from-corpus", action="store_true",
                        help="also treat corpus programs as spellable "
                             "evidence; widens the baseline but usually "
                             "washes out the distinguishing feature")
    arguments = parser.parse_args()

    compiler = pathlib.Path(arguments.compiler).resolve()
    if not compiler.is_file():
        print(f"compiler not found: {compiler}")
        return 2

    programs = generated_programs()
    programs += corpus_programs([pathlib.Path(root)
                                 for root in arguments.corpus])

    outcomes: list[Outcome] = []
    with tempfile.TemporaryDirectory(prefix="gti-decline-census-") as raw:
        workdir = pathlib.Path(raw)
        for index, program in enumerate(programs):
            outcomes.append(classify(compiler, arguments.cxx, program,
                                     workdir, index))

    census = report(outcomes, arguments.learn_from_corpus)

    if arguments.baseline is None:
        return 1 if census["totals"]["decline"] else 0
    if arguments.update:
        arguments.baseline.write_text(
            json.dumps(census, indent=2, sort_keys=True) + "\n",
            encoding="utf-8")
        print(f"\nwrote {arguments.baseline}")
        return 0
    if not arguments.baseline.is_file():
        print(f"\nbaseline not found: {arguments.baseline}")
        return 2
    recorded = json.loads(arguments.baseline.read_text(encoding="utf-8"))
    if recorded == census:
        print("\ncensus matches the baseline")
        return 0
    print("\ncensus differs from the baseline; rerun with --update after "
          "reviewing")
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
