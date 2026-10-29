#!/usr/bin/env python3
"""Alternative source-library cleanup CONTRACT checks, never native witnesses.

Owned-argv LLVM execution/admission is not proved by this harness. It only
uses the public driver's --emit-mir path, and preserves every runtime obligation
as unrun. The stock library is copied, never modified. No post-seal MIR edits,
new native bindings, compiler-private access exemptions or failure seams exist.
"""

from __future__ import annotations

import os
from pathlib import Path
import re
import shutil
import sys
import tempfile

import llvm_coverage_evidence_test as audit
import llvm_readiness_collect as collector


SCHEMA = "gti.llvm.hosted-cleanup-source-contract/v1"
TRACE_PATH = "std/detail_llvm_hosted_cleanup_trace.gti"
CTOR = "    implicit string(string_view value) : data(detail::terminated_prefix_owner(value.size())) {\n"
MOVE = "    string(string&& other) = default;\n"
STRING_INCLUDE = "import std.iterator;\n"
MEMORY_INCLUDE = "import std.meta;\n"
RELEASE = """  void release() mut {
    std::detail::raw_allocation previous = this.take();
    gti_internal::process_resource process{};
    unsafe { process.release(previous); }
  }
"""
PRIMARY = """      if (value == "fixture-primary") {
        gti_hosted_cleanup_fixture::write("primary-trigger\\n");
        std::detail::panic("hosted cleanup fixture primary");
      }
"""
SECONDARY = """    ~string() {
      gti_hosted_cleanup_fixture::write("drop:");
      gti_hosted_cleanup_fixture::write(this.view());
      gti_hosted_cleanup_fixture::write("\\n");
      if (this.view() == "fixture-secondary") {
        std::detail::panic("hosted cleanup fixture secondary");
      }
    }
"""
POST_RELEASE = """    if (previous.address != nullptr) {
      gti_hosted_cleanup_fixture::write("lease-release-returned\\n");
    }
"""


def replace_once(text: str, anchor: str, replacement: str) -> str:
    audit.require(text.count(anchor) == 1, "source transformation anchor must occur exactly once")
    return text.replace(anchor, replacement, 1)


def inventory(directory: Path) -> dict[str, str]:
    result = {}
    for source in sorted(directory.rglob("*")):
        audit.require(not source.is_symlink(), "source-library fixture cannot follow symlinks")
        if source.is_file():
            result[str(source.relative_to(directory))] = audit.file_digest(source)
    return result


def transformed_sources(root: Path, *, failures: bool, release_marker: bool) -> dict[str, str]:
    string = (root / "stdlib/std/string.gti").read_text(encoding="utf-8")
    memory = (root / "stdlib/std/detail/memory_storage.gti").read_text(encoding="utf-8")
    # Even a no-trigger/control setup validates the uninstrumented anchors.
    for text, anchors in ((string, (CTOR, MOVE, STRING_INCLUDE)),
                          (memory, (MEMORY_INCLUDE, RELEASE))):
        for anchor in anchors:
            audit.require(text.count(anchor) == 1, "unmodified source anchor differs")
    if failures:
        string = replace_once(string, STRING_INCLUDE, STRING_INCLUDE +
            'import "detail/panic.gti";\nimport "detail_llvm_hosted_cleanup_trace.gti";\n')
        string = replace_once(string, CTOR, CTOR + PRIMARY)
        string = replace_once(string, MOVE, MOVE + SECONDARY)
    if release_marker:
        audit.require(failures, "post-release markers are an optional alternative-library observation")
        memory = replace_once(memory, MEMORY_INCLUDE,
                              MEMORY_INCLUDE + 'import "../detail_llvm_hosted_cleanup_trace.gti";\n')
        modified_release = replace_once(RELEASE, "    unsafe { process.release(previous); }\n",
                                       "    unsafe { process.release(previous); }\n" + POST_RELEASE)
        memory = replace_once(memory, RELEASE, modified_release)
        audit.require(modified_release.index("this.take()") <
                      modified_release.index("process.release(previous)") <
                      modified_release.index("previous.address != nullptr") <
                      modified_release.index("lease-release-returned"), "source release order changed")
    return {"std/string.gti": string, "std/detail/memory_storage.gti": memory}


def anchor_controls(root: Path) -> None:
    for source, anchors in ((root / "stdlib/std/string.gti", (CTOR, MOVE, STRING_INCLUDE)),
                            (root / "stdlib/std/detail/memory_storage.gti", (MEMORY_INCLUDE, RELEASE))):
        text = source.read_text(encoding="utf-8")
        for anchor in anchors:
            for changed in (text.replace(anchor, "", 1), text + anchor):
                try:
                    replace_once(changed, anchor, anchor)
                except AssertionError:
                    continue
                raise AssertionError("missing/multiple source anchor was accepted")


def validate_inventory(original: dict, transformed: dict, expected: set[str]) -> None:
    audit.require(set(transformed) - set(original) == {TRACE_PATH} and
                  not set(original) - set(transformed) and
                  {path for path in original if original[path] != transformed[path]} == expected,
                  "a source transformation changed unrelated library files")


def field(line: str, name: str) -> str | None:
    match = re.search(r"(?:^|[ ,{])" + re.escape(name) + r"=([^ ,}\n]+)", line)
    return match.group(1) if match else None


def parse_mir(text: str) -> dict:
    lines = text.splitlines()
    audit.require(lines and re.fullmatch(r"mir-v\d+ valid=1", lines[0]) is not None,
                  "fixture did not produce verified program MIR")
    groups = {}
    active = None
    operations = {}
    plan = None
    for line in lines:
        if line.startswith("hosted-startup kind="):
            plan = line
        if match := re.match(r"  hosted-operation @(\d+) ", line):
            operations[int(match.group(1))] = {name: field(line, name) for name in (
                "kind", "failure", "block", "instruction", "cleanup-boundary", "failure-record")}
        if match := re.match(r"^(function|constructor|destructor) @(\d+) ", line):
            active = (match.group(1), int(match.group(2)))
            groups[active] = {"header": line, "body": []}
        elif active is not None:
            groups[active]["body"].append(line)
    audit.require(plan is not None and field(plan, "kind") == "2", "owned-argument plan is absent")
    start = lines.index("hosted-startup-body")
    owner = {name: field(lines[start + 1], name) for name in ("kind", "owner", "entry")}
    audit.require(owner["kind"] == "7" and owner["owner"] == field(plan, "entry"),
                  "startup body does not belong to its actual entry")
    blocks = {}
    current = None
    for line in lines[start + 2:]:
        if match := re.match(r"  bb(\d+) ", line):
            current = {"active_failure": field(line, "active-failure"), "instructions": {}}
            blocks[f"bb{match.group(1)}"] = current
        elif current is not None and (match := re.match(r"    i(\d+) ([^ ]+)", line)):
            current["instructions"][f"i{match.group(1)}"] = {"kind": match.group(2), **{
                name: field(line, name) for name in ("hosted-operation", "constructor", "destination",
                                                     "cleanup-boundary-end")}}
        elif current is not None and (match := re.match(r"    term (\d+) ", line)):
            current["term"] = {"kind": match.group(1), **{
                name: field(line, name) for name in ("invoke", "failure", "target", "else")}}
    return {"version": lines[0], "owner": owner, "plan": plan,
            "operations": operations, "blocks": blocks, "groups": groups}


def calls(group: dict) -> set[int]:
    return {int(value) for line in group["body"] if (value := field(line, "function")) not in (None, "-")}


def literal_owner(mir: dict, literal: str) -> tuple[tuple[str, int], dict]:
    needle = literal.encode("utf-8").hex()
    found = [(key, group) for key, group in mir["groups"].items()
             if any(needle in line for line in group["body"])]
    audit.require(len(found) == 1, f"source literal must belong to one selected body: {literal}")
    return found[0]


def contract(mir: dict, *, failures: bool, release_marker: bool) -> dict:
    groups = mir["groups"]
    native = {key[1] for key, group in groups.items() if key[0] == "function" and
              field(group["header"], "symbol") == "gti_rt_write_stdout_byte"}
    wrappers = {key[1] for key, group in groups.items() if key[0] == "function" and calls(group) & native}
    traces = [(key, group) for key, group in groups.items() if key[0] == "function" and calls(group) & wrappers]
    audit.require(len(native) == len(wrappers) == len(traces) == 1, "trace did not use the existing byte-write chain")
    trace_key, trace = traces[0]
    audit.require(calls(trace) == wrappers, "trace added an unexpected call, owner allocation or panic")
    trace_effects = {name: field(trace["header"], name) for name in
                     ("allocates", "runtime", "traps", "may-raise-defined-failure")}
    checks = [{"operation": field(line, "operation"), "intrinsic": field(line, "intrinsic")}
              for line in trace["body"] if line.startswith("    i") and "failure-origins=[]" not in line]
    result = {"owner": mir["owner"], "mir_version": mir["version"],
              "trace": {"function": trace_key[1], "wrapper": sorted(wrappers), "native": sorted(native),
                        "conservative_effects": trace_effects, "checked_operations": checks},
              "source_panics": {}, "cleanup_edges": []}
    if not failures:
        return result
    primary_key, primary = literal_owner(mir, "hosted cleanup fixture primary")
    secondary_key, secondary = literal_owner(mir, "hosted cleanup fixture secondary")
    audit.require(primary_key[0] == "constructor" and secondary_key[0] == "destructor",
                  "intended source failures moved to instrumentation or the entry body")
    shared_panic = (calls(primary) & calls(secondary)) - {trace_key[1]}
    audit.require(len(shared_panic) == 1, "primary and secondary do not call one existing source panic")
    panic_id = next(iter(shared_panic))
    panic = groups[("function", panic_id)]
    audit.require(field(panic["header"], "noreturn") == "1" and
                  field(panic["header"], "track-caller") == "1" and
                  field(panic["header"], "definition") == "source",
                  "shared source panic identity/attribution contract changed")
    result["source_panics"] = {"primary_constructor": primary_key[1],
        "secondary_destructor": secondary_key[1], "shared_source_panic": panic_id}
    starts = [op for op in mir["operations"].values() if op["kind"] == "construct-argument-string"]
    audit.require(len(starts) == 1, "startup has no exact selected string constructor")
    selected = mir["blocks"][starts[0]["block"]]["instructions"][starts[0]["instruction"]]
    audit.require(selected["constructor"] == str(primary_key[1]), "startup did not select the intended source constructor")
    for operation in mir["operations"].values():
        if operation["kind"] != "drop-failure-cleanup":
            continue
        cleanup = mir["blocks"][operation["block"]]
        drop = cleanup["instructions"][operation["instruction"]]
        invoke = cleanup["term"]
        audit.require(drop["kind"] == "drop" and f"p{drop['destination']}" == field(mir["plan"], "vector-place") and
                      invoke["kind"] == "4" and invoke["invoke"] == operation["instruction"],
                      "failure cleanup did not invoke the initialized argv owner drop")
        normal = mir["blocks"][invoke["target"]]
        failed = mir["blocks"][invoke["else"]]
        ends = [item for item in mir["operations"].values()
                if item["kind"] == "end-failure-cleanup" and item["block"] == invoke["target"]]
        terminals = [item for item in mir["operations"].values()
                     if item["kind"] == "terminate-cleanup-failure" and item["block"] == invoke["else"]]
        audit.require(len(ends) == len(terminals) == 1 and normal["term"]["kind"] == "9" and
                      normal["term"]["failure"] == cleanup["active_failure"] and
                      failed["term"]["kind"] == "10" and failed["term"]["failure"] == invoke["failure"] and
                      failed["active_failure"] == cleanup["active_failure"],
                      "primary/secondary records or normal/terminal cleanup routes changed")
        result["cleanup_edges"].append({"drop_block": operation["block"],
            "drop_instruction": operation["instruction"], "primary": cleanup["active_failure"],
            "secondary": invoke["failure"], "normal": invoke["target"], "terminal": invoke["else"],
            "end_instruction": ends[0]["instruction"]})
    audit.require(result["cleanup_edges"], "alternative source did not produce hosted cleanup edges")
    if release_marker:
        key, release = literal_owner(mir, "lease-release-returned")
        audit.require(key[0] == "function" and trace_key[1] in calls(release), "post-release source marker was not lowered")
        result["post_release_source_function"] = key[1]
    return result


def main() -> None:
    gti, helper, root, build = (Path(value).resolve() for value in sys.argv[1:5])
    report = audit.ledger(helper)
    audit.require(report["build"]["configuration"] in collector.OPTIMIZED and
                  not audit.repository_source_errors(report, audit.repository_source(root)),
                  "source/MIR fixture check needs the matching optimized compiler")
    original = inventory(root / "stdlib")
    anchor_controls(root)
    fixtures = root / "tests/fixtures"
    source = fixtures / "llvm_hosted_cleanup_entry.gti"
    trace_source = fixtures / "llvm_hosted_cleanup_trace.gti"
    source_hash, trace_hash, compiler_hash = map(audit.file_digest, (source, trace_source, gti))
    evidence = Path(tempfile.mkdtemp(prefix="gti_llvm_hosted_cleanup_contract_", dir=build))
    record = {"schema": SCHEMA, "source": report["source"], "build": report["build"],
        "compiler_sha256": compiler_hash, "fixture_sha256": source_hash, "trace_sha256": trace_hash,
        "canonical_repository_observation": False, "stock_cleanup_reachability": "unproven",
        "owned_argv_llvm_execution": "not-observed-by-this-contract-harness", "hosted_gap_rows_closed": False,
        "runtime_obligations": {key: "unrun" for key in ("trace", "reverse_cleanup", "main_not_entered",
            "primary_secondary_origins", "single_termination", "allocator_counts", "double_cleanup")},
        "planned_runtime_cases": {
            "no-trigger": {"arguments": ["prior-one", "prior-two"], "main_entered": True},
            "primary": {"arguments": ["prior-one", "prior-two", "fixture-primary"], "main_entered": False},
            "secondary": {"arguments": ["prior-one", "fixture-secondary", "fixture-primary"], "main_entered": False}},
        "trace_limits": {"post_release_marker": "source release call returned, not allocator/free/byte counts",
            "allocation": "no owning source string in helper; conservative MIR and libc buffering remain",
            "bounds": "immutable length N; index<N guards access; final increment<=N; fixed markers/args ASCII",
            "write_error": "nonzero byte-write status returns; incomplete output must fail future oracle"},
        "cpp_comparator": "unrun", "evidence_directory": str(evidence), "variants": {}}
    for name, failures, release_marker in (("baseline", False, False),
            ("source-panics", True, False), ("source-panics-with-release-marker", True, True)):
        directory = evidence / name
        directory.mkdir()
        library = directory / "stdlib"
        shutil.copytree(root / "stdlib", library)
        audit.require(inventory(library) == original, "source library copy differs before transformation")
        modified = transformed_sources(root, failures=failures, release_marker=release_marker)
        for relative, text in modified.items():
            (library / relative).write_text(text, encoding="utf-8")
        shutil.copyfile(trace_source, library / TRACE_PATH)
        transformed = inventory(library)
        expected = {"std/string.gti"} if failures else set()
        if release_marker:
            expected.add("std/detail/memory_storage.gti")
        validate_inventory(original, transformed, expected)
        changed = dict(transformed)
        changed["prelude.gti"] = "0" * 64
        try:
            validate_inventory(original, changed, expected)
        except AssertionError:
            pass
        else:
            raise AssertionError("untouched source-file drift was accepted")
        variant = {"alternative_source_library": True, "source_panics": failures,
                   "post_release_marker": release_marker, "original_inventory": original,
                   "transformed_inventory": transformed, "inventory_sha256": audit.digest(transformed), "modes": {}}
        variant["shared_panic_source"] = {"public_wrapper": "std/panic.gti",
            "core": "std/detail/panic.gti", "core_sha256": transformed["std/detail/panic.gti"],
            "implementation_unchanged": transformed["std/detail/panic.gti"] == original["std/detail/panic.gti"]}
        if failures:
            source_lines = modified["std/string.gti"].splitlines()
            variant["declared_source_sites"] = {name: {"path": "std/string.gti",
                "line": next(index for index, line in enumerate(source_lines, 1)
                             if message in line), "runtime_attribution": "unrun"}
                for name, message in (("primary", "hosted cleanup fixture primary"),
                                      ("secondary", "hosted cleanup fixture secondary"))}
        collector.write_json(directory / "library.json", variant)
        for level in ("O0", "O3"):
            work = directory / level
            work.mkdir()
            command = [str(gti), str(source), "--emit-mir", "--std", "c++23", "--execution-profile", "single-threaded",
                       f"-{level}", "-o", str(work / "verified.mir")]
            # The original bounded runner deliberately inherits the environment;
            # this one explicit resource selection is scoped to this child.
            saved = os.environ.get("GTI_STDLIB_PATH")
            os.environ["GTI_STDLIB_PATH"] = str(library)
            try:
                observed = collector.run_probe(command, root, work, timeout=120)
            finally:
                if saved is None:
                    os.environ.pop("GTI_STDLIB_PATH", None)
                else:
                    os.environ["GTI_STDLIB_PATH"] = saved
            collector.write_json(work / "process.json", observed)
            audit.require(observed["status"] == "completed" and observed["returncode"] == 0,
                          f"alternative source verification failed; inspect {work / 'stderr'}")
            output = work / "verified.mir"
            audit.require(output.is_file() and output.stat().st_size <= 32 * 1024 * 1024,
                          "missing/oversized verified MIR artifact")
            projected = contract(parse_mir(output.read_text(encoding="utf-8")),
                                 failures=failures, release_marker=release_marker)
            collector.write_json(work / "contract.json", projected)
            request = {"purpose": "public-driver-mir-emission", "entry": str(source),
                "entry_required": False, "definition_closure_required": False, "runtime_profile": "hosted",
                "target": collector.canonical_host_target(helper, "single-threaded"),
                "optimization": level, "cpp_standard": "c++23", "debug_info": False,
                "configuration_flags": [], "declared_configuration_flags": [], "package_sources": [],
                "raw_pointer_audit": "off", "program_kind": "executable", "test_suite": False,
                "stdlib_selection": str(library), "prelude": str(library / "prelude.gti"),
                "source_library_inventory_sha256": audit.digest(transformed),
                "backend": "MirBackend-inspection-only",
                "driver_request_backend_default": "cpp; not dispatched by compileToMir"}
            variant["modes"][level] = {"process": observed, "mir_sha256": audit.file_digest(output),
                "stdout_sha256": audit.file_digest(work / "stdout"),
                "stderr_sha256": audit.file_digest(work / "stderr"),
                "contract": projected, "request": request, "request_sha256": audit.digest(request),
                "native_execution": "unrun"}
            audit.require(inventory(library) == transformed, "source library changed during verification")
        collector.write_json(directory / "library.json", variant)
        record["variants"][name] = variant
    audit.require(inventory(root / "stdlib") == original and audit.file_digest(gti) == compiler_hash and
                  audit.file_digest(source) == source_hash and audit.file_digest(trace_source) == trace_hash and
                  not audit.repository_source_errors(report, audit.repository_source(root)),
                  "compiler/fixture/stock library changed during source/MIR checks")
    collector.write_json(evidence / "evidence.json", record)
    collector.write_json(build / "gti_llvm_hosted_cleanup_source_contract.json", record)
    print(f"Alternative source/MIR contracts passed (runtime obligations UNRUN): {evidence}")


if __name__ == "__main__":
    main()
