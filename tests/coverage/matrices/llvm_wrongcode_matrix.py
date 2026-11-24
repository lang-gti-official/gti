#!/usr/bin/env python3
"""Self-checking matrices for silent LLVM wrong code.

Every generated program verifies its own results against values derived from
GTI semantics (the literal inputs it was generated from), never from a
backend's output, and counts destructions of tracked objects. A program returns
non-zero on any mismatch, so no second backend is needed as an oracle. Each
cell is built with the LLVM backend at every selected level (O0 and O2 by
default) and run under a watchdog. A cell whose build succeeds but whose run
fails, crashes, hangs or prints something other than `ok` is wrong code; a
cell whose levels disagree (one runs, another refuses or runs differently) is
a level divergence. Both fail the run, as do internal errors and the other
hard verdicts of matrix_support.

    tests/coverage/matrices/llvm_wrongcode_matrix.py build-release/gti \
        [--levels O0,O2,O3] [--matrix transfer ...] [--jobs 4] \
        [--out DIR] [--json report.json] [--baseline old.json]

Matrices (axes):
  transfer   owned value kind x transfer x class shape
  containers owned element kind x container x operation
  flow       owned element kind x control-flow ownership pattern
  hierarchy  polymorphic holder of owned fields x operation
  bases      concrete-base hierarchy with owned fields x operation
  enums      payload enum shape x operation
  callables  erased callable holding owned state x operation
  order      class shape x end of lifetime, checking destruction order
  dispatch   hierarchy x helper x owner, checking virtual calls during
             construction and destruction
  library    standard-library containers holding owned values
  poly_containers  vectors and maps of unique and shared polymorphic owners
             x insert, erase, sort, move, pass and replace
  callbacks  erased callbacks capturing objects x fields, interfaces,
             vectors, maps, factories and polymorphic owners
  sum_types  optional, expected and payload enums of class and hierarchy
             types x return, move, match, store and consume
  text_relays  string views and spans relayed through methods, virtuals,
             interfaces, generics and operators x owner
  generics   constrained generic functions, pools, factories, visitors and a
             generic derived class x interface and class hierarchies
  algorithms sort, search, count, predicates, visit, reverse, transform and
             max over vectors of passive, owning and polymorphic elements
  threads    thread results, relays, worker vectors, shared mutexes and
             parallel_invoke x class, text, owner and payload-enum payloads
  raii       cleanup order under early returns, loops, switch cases, branch
             locals, condition temporaries and returned locals

RFC 0013 modules (expected traces, exit statuses and diagnostics derived
from docs/language/; see each module's docstring for its contract):
  weak_owners      shared and weak handles x holder x operation, cycles
  thread_local_owners  TLS storage form x payload x thread, first-use order
  deferred         scope guards x exit (return, loops, `?`, failure)
  propagation      `?` carrier (expected, std::from, tryable, optional) x
                   position (temporaries, loops, init statements, lambdas)
  panics           failure source x cleanup context; status 70 and report
  payload_binding  switch, if case, init statements, structured bindings
  owning_enum_transport  enum shape x arrays, closures, callbacks, packs
  borrowed_elements  container x element access through owner-tied cursors
  conversions      implicit and explicit conversions, default construction,
                   protocol concepts
  allocation       storage, arena, pool and resource-vector slot lifetimes
  lifetimes        region-annotated functions and classes, invalid siblings

A module's cells may carry an expectation (exact stdout, exit status, a
stderr substring, or the diagnostic an invalid sibling must get) instead of
`ok`. `--negative-controls` builds each module's NEGATIVE_CONTROL, a
deliberately broken cell, and fails unless the oracle flags every one.
"""
import concurrent.futures
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import matrix_support  # noqa: E402
from llvm_wrongcode import (algorithms, arrays, bases, callables,  # noqa: E402
                            containers, error_chains, formatting,
                            dispatch, enums, flow, hierarchy, library,
                            callbacks, generics, nested_generics, operators,
                            order, poly_containers, ranges, static_state,
                            raii, sum_types, text_relays, threads,
                            transfer, trees, weak_owners, panics,
                            thread_local_owners, deferred,
                            propagation, payload_binding,
                            owning_enum_transport, borrowed_elements,
                            conversions, allocation, lifetimes)

MATRICES = {"transfer": transfer, "containers": containers, "flow": flow,
            "hierarchy": hierarchy, "bases": bases, "enums": enums,
            "callables": callables, "order": order, "dispatch": dispatch,
            "library": library, "poly_containers": poly_containers,
            "callbacks": callbacks, "sum_types": sum_types,
            "text_relays": text_relays, "generics": generics,
            "algorithms": algorithms, "threads": threads, "raii": raii,
            "operators": operators, "ranges": ranges,
            "nested_generics": nested_generics, "formatting": formatting,
            "arrays": arrays, "trees": trees, "error_chains": error_chains,
            "static_state": static_state, "weak_owners": weak_owners,
            "panics": panics,
            "thread_local_owners": thread_local_owners,
            "deferred": deferred,
            "propagation": propagation,
            "payload_binding": payload_binding,
            "owning_enum_transport": owning_enum_transport,
            "borrowed_elements": borrowed_elements,
            "conversions": conversions,
            "allocation": allocation,
            "lifetimes": lifetimes}


def options(parser):
    parser.add_argument("--matrix", nargs="*", choices=sorted(MATRICES),
                        default=sorted(MATRICES))
    parser.add_argument("--jobs", type=int, default=4)
    parser.add_argument("--negative-controls", action="store_true",
                        help="instead of the cells, build each selected "
                             "module's NEGATIVE_CONTROL (a deliberately "
                             "broken cell) and fail unless every one is "
                             "flagged with a hard verdict")


def main():
    matrix = matrix_support.Matrix(__doc__.split("\n\n")[0], "llvm-wrongcode",
                                   default_levels="O0,O2", extra=options)
    sources = []
    expectations = {}
    for name in matrix.args.matrix:
        directory = matrix.out / name
        directory.mkdir(parents=True, exist_ok=True)
        module = MATRICES[name]
        cells = list(module.cells())
        if matrix.args.negative_controls:
            if not hasattr(module, "NEGATIVE_CONTROL"):
                continue
            target, old, new = module.NEGATIVE_CONTROL
            cells = [(f"{cell[0]}-control", cell[1].replace(old, new, 1), *cell[2:])
                     for cell in cells if cell[0] == target and old in cell[1]]
            if not cells:
                raise SystemExit(f"{name}: negative control does not apply")
        for cell in cells:
            # A cell is (name, text), or (name, text, expectation): its exact
            # stdout, exit status and a standard-error report substring for
            # one that checks a defined failure, or the diagnostic code an
            # invalid sibling must be rejected with.
            path = directory / f"{cell[0]}.gti"
            path.write_text(cell[1], encoding="utf-8")
            sources.append(path)
            expectations[path] = cell[2] if len(cell) > 2 else {}

    def classify(source):
        expect = expectations[source]
        verdict = matrix.classify(
            source, flags=("--execution-profile", "concurrent"),
            expect_stdout=expect.get("stdout", "ok\n"),
            expect_returncode=expect.get("returncode", 0),
            expect_stderr=expect.get("stderr"),
            expect_diagnostic=expect.get("diagnostic"))
        return source, verdict

    counts = {"wrong": 0, "diverge": 0, "other": 0, "unsupported": 0}
    failures = []
    with concurrent.futures.ThreadPoolExecutor(matrix.args.jobs) as pool:
        for source, verdict in pool.map(classify, sources):
            cell = f"{source.parent.name}/{source.stem}"
            levels = matrix.records[f"{cell}.gti"]["levels"]
            print(f"{cell:48} " + " ".join(
                f"{level}={levels[level]:24}" for level in matrix.levels),
                flush=True)
            if verdict.startswith("UNSUPPORTED"):
                counts["unsupported"] += 1
            if not matrix_support.hard(verdict):
                continue
            failures.append((cell, verdict))
            if verdict.startswith(("RUN", "WRONG")):
                counts["wrong"] += 1
            elif verdict.startswith("DIVERGE"):
                counts["diverge"] += 1
            else:
                counts["other"] += 1
    if matrix.args.negative_controls:
        # A control passes when the oracle flags it; report the ones it missed.
        missed = [str(source.relative_to(matrix.out)) for source in sources
                  if not any(cell == f"{source.parent.name}/{source.stem}"
                             for cell, _ in failures)]
        print(f"{len(sources)} negative controls: {len(sources) - len(missed)} "
              f"flagged, {len(missed)} missed")
        for cell in missed:
            print(f"  MISSED  {cell}")
        matrix.finish([])
        return 1 if missed else 0
    print(f"{len(sources)} cells at {','.join(matrix.levels)}: "
          f"{counts['wrong']} with LLVM wrong code, {counts['diverge']} level "
          f"divergences, {counts['other']} other hard failures, "
          f"{counts['unsupported']} declined by the LLVM backend")
    for cell, verdict in failures:
        print(f"  {verdict}  {cell}")
    return matrix.finish(failures)


if __name__ == "__main__":
    sys.exit(main())
