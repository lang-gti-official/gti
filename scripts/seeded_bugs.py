#!/usr/bin/env python3
"""Seeded-bug sample: how many injected single-point faults does a test command catch?

    seeded_bugs.py sample --build build -n 180 --seed 1 --output sample.json
    seeded_bugs.py run --build build -n 180 --seed 1 --slice 0/6 \\
        --build-command 'cmake --build build --target gti ...' \\
        --test-command 'ctest --test-dir build --stop-on-failure -L feedback-fast' \\
        --output slice-0.json
    seeded_bugs.py merge slice-*.json --output seeded-bugs.json
    seeded_bugs.py --self-test

Mutants are drawn deterministically (seeded) from the first-party C++ sources
the build compiles (compile_commands.json, restricted to `src/`), stratified by
directory in proportion to each directory's mutation sites, with operators
taking turns inside a directory so rare kinds are represented. Each is one small
textual edit placed with a comment/string/preprocessor-aware scan:

    rel-flip     == <-> !=, < <-> >=, > <-> <=  (binary operators with spaces)
    rel-boundary < -> <=, <= -> <, > -> >=, >= -> >
    off-by-one   an integer literal compared against gains one
    logic-flip   && <-> ||
    negate       if (c) / while (c) -> (!(c))
    skip-exit    an `if (c)` guarding return/continue/break/throw -> (false && (c))
    drop-diag    a statement that emits a diagnostic -> (void)0;

`run` builds the unmutated tree and requires the test commands to pass first,
then for each mutant: applies it, runs the build command, checks that the
mutated source's object was actually rebuilt, runs the test commands in order
until one fails, records the outcome, and restores the file (also on error or
SIGTERM). Outcomes: killed, survived, build-failed, not-built, not-run (time
budget exhausted). The kill rate is killed / (killed + survived).

Mutant ids are stable for the same source text, so two test commands can be
compared on the same mutants with `run --replay earlier.json`.
"""

import argparse
import datetime
import hashlib
import json
import os
import random
import re
import shlex
import signal
import subprocess
import sys
import tempfile
import time
from pathlib import Path

SCHEMA = 1
SCOPE = "src/"
SOURCE_SUFFIXES = (".cpp", ".cc", ".cxx")
OPERATORS = ("rel-flip", "rel-boundary", "off-by-one", "logic-flip", "negate", "skip-exit", "drop-diag")
REL_FLIP = {"==": "!=", "!=": "==", "<": ">=", ">=": "<", ">": "<=", "<=": ">"}
REL_BOUNDARY = {"<": "<=", "<=": "<", ">": ">=", ">=": ">"}
DIAGNOSTIC_CALLEE = re.compile(
    r"(?:^|[.>:])(?:(?:report|emit|add|raise|record|push)\w*(?:Error|Diagnostic|Warning)s?\w*|diagnose\w*|\w*_error)$")
DIAGNOSTIC_RECEIVER = re.compile(r"(?:^|[.>])\w*[Dd]iagnostics?(?:\.|->)(?:emplace_back|push_back|insert|add|append|report|emit)$")
FAILED_TEST = re.compile(r"^\s*\d+\s+-\s+(\S+)\s+\((?:Failed|Timeout|SEGFAULT|Subprocess aborted|Exception|Child aborted|ILLEGAL|BUS|SIGTRAP|OTHER_FAULT|Not Run)[^)]*\)")


# --- scanning ---------------------------------------------------------------

def mask(text):
    """`text` with comments, string/char literals and preprocessor lines blanked.

    Offsets and newlines are preserved, so a match in the mask is a match in
    the source at the same offset."""
    out, i, n = list(text), 0, len(text)

    def blank(start, end):
        for k in range(start, end):
            if out[k] != "\n":
                out[k] = " "

    line_start = True
    while i < n:
        c = text[i]
        if line_start and c in " \t":
            i += 1
            continue
        if line_start and c == "#":
            j = i
            while j < n and not (text[j] == "\n" and text[j - 1] != "\\"):
                j += 1
            blank(i, j)
            i = j
            continue
        line_start = c == "\n"
        if text.startswith("//", i):
            j = text.find("\n", i)
            j = n if j < 0 else j
            blank(i, j)
            i = j
        elif text.startswith("/*", i):
            j = text.find("*/", i + 2)
            j = n if j < 0 else j + 2
            blank(i, j)
            i = j
        elif c == "R" and text.startswith('R"', i) and (i == 0 or not (text[i - 1].isalnum() or text[i - 1] == "_")
                                                        or text[i - 1] in "u8LU"):
            open_paren = text.find("(", i + 2)
            delimiter = text[i + 2:open_paren]
            j = text.find(")" + delimiter + '"', open_paren)
            j = n if j < 0 else j + len(delimiter) + 2
            blank(i, j)
            i = j
        elif c == '"' or (c == "'" and not (i > 0 and text[i - 1].isalnum() and _in_number(text, i))):
            j = i + 1
            while j < n and text[j] != c and text[j] != "\n":
                j += 2 if text[j] == "\\" else 1
            blank(i, min(j + 1, n))
            i = j + 1
        else:
            i += 1
    return "".join(out)


def _in_number(text, i):
    """Whether the quote at `i` is a C++14 digit separator."""
    j = i
    while j > 0 and (text[j - 1].isalnum() or text[j - 1] in "'_."):
        j -= 1
    return text[j].isdigit()


def matching_paren(masked, open_index):
    depth = 0
    for k in range(open_index, len(masked)):
        if masked[k] == "(":
            depth += 1
        elif masked[k] == ")":
            depth -= 1
            if depth == 0:
                return k
    return -1


def plain_condition(condition):
    """A condition that is an expression (no declaration, init-statement or assignment)."""
    stripped = condition.strip()
    if not stripped or ";" in stripped or re.match(r"(?:const\s+)?auto\b|[A-Za-z_][\w:<>,\s]*[\s*&]+\w+\s*[={]", stripped):
        return False
    return not re.search(r"(?<![=!<>+\-*/%&|^])=(?!=)", stripped)


def sites(path, relative):
    """Every mutation site of one source file, as dicts with offset/length/replacement."""
    text = path.read_text(errors="replace")
    masked = mask(text)
    found = []

    def add(start, end, replacement, operator):
        line = text.count("\n", 0, start) + 1
        column = start - (text.rfind("\n", 0, start) + 1) + 1
        found.append({"file": relative, "offset": start, "length": end - start, "original": text[start:end],
                      "replacement": replacement, "operator": operator, "line": line, "column": column})

    for match in re.finditer(r"(?<=[\w)\]]) (==|!=|<=|>=|<|>) (?=[\w(!\-*&\"'])", masked):
        op = match.group(1)
        start = match.start(1)
        add(start, start + len(op), REL_FLIP[op], "rel-flip")
        if op in REL_BOUNDARY:
            add(start, start + len(op), REL_BOUNDARY[op], "rel-boundary")
    for match in re.finditer(r"(?<=[\w)\]]) (?:==|!=|<=|>=|<|>) (\d+)(?![\w.'])", masked):
        add(match.start(1), match.end(1), str(int(match.group(1)) + 1), "off-by-one")
    for match in re.finditer(r"(?<=\S) (&&|\|\|) (?=\S)", masked):
        add(match.start(1), match.end(1), "||" if match.group(1) == "&&" else "&&", "logic-flip")
    for match in re.finditer(r"\b(if|while) \(", masked):
        if masked[match.start() - 1:match.start()] == "#":
            continue
        open_index = match.end() - 1
        close = matching_paren(masked, open_index)
        if close < 0:
            continue
        condition = text[open_index + 1:close]
        if not plain_condition(masked[open_index + 1:close]):
            continue
        add(open_index, close + 1, "(!(" + condition + "))", "negate")
        if match.group(1) == "if":
            after = masked[close + 1:close + 200].lstrip()
            if after.startswith("{"):
                after = after[1:].lstrip()
            if re.match(r"(return|continue|break|throw)\b", after):
                add(open_index, close + 1, "(false && (" + condition + "))", "skip-exit")
    callee = r"((?:[A-Za-z_]\w*(?:\.|->|::))*[A-Za-z_]\w*)\("
    for match in re.finditer(callee, masked):
        name = match.group(1)
        if not (DIAGNOSTIC_CALLEE.search(name) or DIAGNOSTIC_RECEIVER.search(name)):
            continue
        before = masked[:match.start()].rstrip()
        if not before or before[-1] not in ";{}":
            continue
        close = matching_paren(masked, match.end() - 1)
        if close < 0 or not masked[close + 1:].lstrip().startswith(";"):
            continue
        end = masked.index(";", close + 1) + 1
        add(match.start(), end, "(void)0;", "drop-diag")
    for site in found:
        key = f"{site['file']}\0{site['offset']}\0{site['operator']}\0{site['replacement']}"
        site["id"] = hashlib.sha1(key.encode()).hexdigest()[:12]
    return found


# --- sampling ---------------------------------------------------------------

def compiled_sources(root, build):
    """{repository-relative source: [object paths]} for in-scope compiled sources."""
    database = Path(build) / "compile_commands.json"
    if not database.is_file():
        raise ValueError(f"{database} is missing; configure with CMAKE_EXPORT_COMPILE_COMMANDS")
    sources = {}
    for entry in json.loads(database.read_text()):
        path = Path(entry["directory"], entry["file"]).resolve()
        try:
            relative = path.relative_to(root).as_posix()
        except ValueError:
            continue
        if not relative.startswith(SCOPE) or path.suffix not in SOURCE_SUFFIXES:
            continue
        output = entry.get("output")
        objects = sources.setdefault(relative, [])
        if output:
            objects.append(str(Path(entry["directory"], output)))
    return sources


def ninja_inputs(build, targets):
    """Sources the named Ninja targets depend on, or None when Ninja cannot say."""
    try:
        listed = subprocess.run(["ninja", "-C", str(build), "-t", "inputs", *targets],
                                capture_output=True, text=True, check=True).stdout
    except (OSError, subprocess.CalledProcessError):
        return None
    return {str(Path(build, line.strip()).resolve()) for line in listed.splitlines() if line.strip()}


def allocate(counts, n):
    """Largest-remainder allocation of n draws over strata, at least one each when n allows."""
    strata = sorted(name for name, count in counts.items() if count)
    if not strata or n <= 0:
        return {}
    total = sum(counts[name] for name in strata)
    floor = 1 if n >= len(strata) else 0
    spare = n - floor * len(strata)
    shares = {name: floor + spare * counts[name] / total for name in strata}
    result = {name: min(int(share), counts[name]) for name, share in shares.items()}
    for name in sorted(strata, key=lambda item: (-(shares[item] - int(shares[item])), item)):
        if sum(result.values()) >= n:
            break
        if result[name] < counts[name]:
            result[name] += 1
    return result


def sample(root, build, n, seed, targets=None):
    sources = compiled_sources(root, build)
    if targets:
        inputs = ninja_inputs(build, targets)
        if inputs is not None:
            sources = {name: objects for name, objects in sources.items() if str(root / name) in inputs}
    by_stratum = {}
    for relative in sorted(sources):
        by_stratum.setdefault(str(Path(relative).parent), []).extend(sites(root / relative, relative))
    plan = allocate({name: len(rows) for name, rows in by_stratum.items()}, n)
    rng = random.Random(seed)
    chosen = []
    for stratum in sorted(plan):
        # Operators take turns inside a directory, so rare kinds (dropped
        # diagnostics) are represented beside the abundant comparisons.
        pools = {}
        for site in by_stratum[stratum]:
            pools.setdefault(site["operator"], []).append(site)
        for pool in pools.values():
            rng.shuffle(pool)
        order = sorted(pools)
        rng.shuffle(order)
        drawn = 0
        while drawn < plan[stratum]:
            for operator in order:
                if drawn < plan[stratum] and pools[operator]:
                    site = pools[operator].pop()
                    chosen.append(dict(site, stratum=stratum, objects=sources[site["file"]]))
                    drawn += 1
    rng.shuffle(chosen)
    return chosen, {name: len(rows) for name, rows in by_stratum.items()}


# --- running ----------------------------------------------------------------

class Interrupted(Exception):
    pass


def run_command(argv, cwd, timeout, log):
    """(status, seconds, output tail); status is the exit code, or 'timeout'."""
    started = time.monotonic()
    with tempfile.TemporaryFile("w+") as capture:
        try:
            process = subprocess.Popen(argv, cwd=cwd, stdout=capture, stderr=subprocess.STDOUT, text=True,
                                       start_new_session=True)
            try:
                status = process.wait(timeout=timeout)
            except subprocess.TimeoutExpired:
                os.killpg(process.pid, signal.SIGKILL)
                process.wait()
                status = "timeout"
        except BaseException:
            if "process" in locals() and process.poll() is None:
                os.killpg(process.pid, signal.SIGKILL)
            raise
        capture.seek(0)
        output = capture.read()
    if log:
        log.write(f"$ {shlex.join(argv)}\n{output}\n[status {status}]\n")
        log.flush()
    return status, round(time.monotonic() - started, 1), output


def failing_test(output, argv):
    tests = [match.group(1) for match in map(FAILED_TEST.match, output.splitlines()) if match]
    return tests[0] if tests else shlex.join(argv)[:160]


def object_times(mutant):
    return {path: (os.stat(path).st_mtime_ns if os.path.exists(path) else None) for path in mutant.get("objects", [])}


def apply(root, mutant):
    path = root / mutant["file"]
    original = path.read_bytes()
    text = original.decode(errors="surrogateescape")
    start, end = mutant["offset"], mutant["offset"] + mutant["length"]
    if text[start:end] != mutant["original"]:
        return None
    path.write_bytes((text[:start] + mutant["replacement"] + text[end:]).encode(errors="surrogateescape"))
    return original


def run_all(root, mutants, build_commands, test_commands, budget, mutant_timeout, log):
    root = Path(root)
    deadline = time.monotonic() + budget if budget else None
    status, seconds, output = run_command(build_commands, root, None, log)
    if status != 0:
        raise ValueError("the unmutated tree does not build:\n" + output[-2000:])
    for command in test_commands:
        status, seconds, output = run_command(command, root, mutant_timeout, log)
        if status != 0:
            raise ValueError(f"the unmutated tree fails `{shlex.join(command)}`:\n" + output[-2000:])
    previous = signal.signal(signal.SIGTERM, lambda *_: (_ for _ in ()).throw(Interrupted()))
    results = []
    try:
        for mutant in mutants:
            row = {key: mutant[key] for key in ("id", "file", "line", "column", "operator", "original",
                                                "replacement", "stratum", "offset", "length")}
            results.append(row)
            if deadline and time.monotonic() > deadline:
                row["status"] = "not-run"
                continue
            before = object_times(mutant)
            original = apply(root, mutant)
            if original is None:
                row["status"] = "stale"
                continue
            try:
                status, row["build_seconds"], output = run_command(build_commands, root, mutant_timeout, log)
                if status != 0:
                    row["status"] = "build-failed"
                    continue
                after = object_times(mutant)
                if before and all(after[path] == before[path] for path in before):
                    row["status"] = "not-built"
                    continue
                row["status"], row["test_seconds"] = "survived", 0.0
                for command in test_commands:
                    status, seconds, output = run_command(command, root, mutant_timeout, log)
                    row["test_seconds"] = round(row["test_seconds"] + seconds, 1)
                    if status != 0:
                        row["status"], row["killed_by"] = "killed", failing_test(output, command)
                        if status == "timeout":
                            row["killed_by"] += " (mutant timeout)"
                        break
            finally:
                (root / mutant["file"]).write_bytes(original)
            print(f"{row['status']:12} {row['operator']:12} {row['file']}:{row['line']}"
                  + (f"  by {row['killed_by']}" if row.get("killed_by") else ""), flush=True)
    finally:
        signal.signal(signal.SIGTERM, previous)
    return results


def summary(results):
    counts = {}
    groups = {"by_stratum": {}, "by_operator": {}}
    for row in results:
        counts[row["status"]] = counts.get(row["status"], 0) + 1
        for key, field in (("by_stratum", "stratum"), ("by_operator", "operator")):
            bucket = groups[key].setdefault(row[field], {})
            bucket[row["status"]] = bucket.get(row["status"], 0) + 1

    def rate(bucket):
        decided = bucket.get("killed", 0) + bucket.get("survived", 0)
        return round(100.0 * bucket.get("killed", 0) / decided, 2) if decided else None

    for key in groups:
        for bucket in groups[key].values():
            bucket["kill_rate"] = rate(bucket)
    return {"mutants": len(results), "counts": counts, "kill_rate": rate(counts), **groups}


def git_head(root):
    try:
        return subprocess.run(["git", "-C", str(root), "rev-parse", "HEAD"], capture_output=True, text=True,
                              check=True).stdout.strip()
    except (OSError, subprocess.CalledProcessError):
        return None


def parse_slice(text):
    index, _, count = (text or "0/1").partition("/")
    index, count = int(index), int(count)
    if not 0 <= index < count:
        raise ValueError("--slice must be i/k with 0 <= i < k")
    return index, count


def choose(args, root):
    if args.replay:
        earlier = json.loads(Path(args.replay).read_text())
        sources = compiled_sources(root, Path(args.build).resolve())
        mutants = [dict(row, objects=sources.get(row["file"], [])) for row in earlier["results"]]
        return mutants, earlier.get("strata", {})
    targets = shlex.split(args.ninja_targets) if args.ninja_targets else None
    return sample(root, Path(args.build).resolve(), args.n, args.seed, targets)


def document(args, root, mutants, strata, results=None):
    index, count = parse_slice(getattr(args, "slice", None))
    body = {"schema": SCHEMA, "kind": "gti-seeded-bugs", "commit": git_head(root), "seed": args.seed,
            "requested": args.n, "slice": [index, count], "strata": strata,
            "generated_at": datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")}
    if results is None:
        body["sample"] = [{key: value for key, value in row.items() if key != "objects"} for row in mutants]
    else:
        body["build_command"] = args.build_command
        body["test_commands"] = args.test_command
        body["results"] = results
        body["summary"] = summary(results)
    return body


def merge(paths):
    parts = [json.loads(Path(path).read_text()) for path in paths]
    if not parts or len({(part["commit"], part["seed"], part["requested"]) for part in parts}) != 1:
        raise ValueError("slices disagree on commit, seed or sample size")
    count = parts[0]["slice"][1]
    indices = sorted(part["slice"][0] for part in parts)
    if len(set(indices)) != len(indices) or any(part["slice"][1] != count for part in parts):
        raise ValueError("slices are duplicated or disagree on the slice count")
    # A slice whose job failed is reported, not silently dropped.
    missing = sorted(set(range(count)) - set(indices))
    results = [row for part in sorted(parts, key=lambda part: part["slice"][0]) for row in part["results"]]
    merged = {key: parts[0][key] for key in ("schema", "kind", "commit", "seed", "requested", "strata",
                                            "build_command", "test_commands")}
    merged.update(slice=[0, 1], slices=count, missing_slices=missing, results=results, summary=summary(results),
                  generated_at=datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"))
    return merged


# --- self-test --------------------------------------------------------------

SAMPLE_SOURCE = r'''#include <vector>
#if defined(X) && Y > 2
#endif
// if (commented < out) return;
static const char* text = "a < b && c == d";
int f(int a, int b, std::vector<int>& diagnostics) {
  if (a < 10) {
    return 0;
  }
  while (a != b && b >= 0) {
    --b;
  }
  if (auto c = a; c == 1) return 2;
  diagnostics.push_back(a);
  reportTypeError(a, b);
  int x = 1'000 > a ? 1 : 0;
  return a == b || x <= 3 ? 1 : 2;
}
'''


def self_test():
    with tempfile.TemporaryDirectory() as directory:
        root = Path(directory).resolve()
        source = root / "src/demo/a.cpp"
        source.parent.mkdir(parents=True)
        source.write_text(SAMPLE_SOURCE)
        masked = mask(SAMPLE_SOURCE)
        assert len(masked) == len(SAMPLE_SOURCE) and masked.count("\n") == SAMPLE_SOURCE.count("\n")
        assert "commented" not in masked and "a < b" not in masked and "#if" not in masked
        assert "1'000 > a" in masked, "a digit separator is not a character literal"
        found = sites(source, "src/demo/a.cpp")
        by = {}
        for site in found:
            by.setdefault(site["operator"], []).append(site)
            assert SAMPLE_SOURCE[site["offset"]:site["offset"] + site["length"]] == site["original"]
        assert {site["original"] for site in by["rel-flip"]} == {"<", "!=", ">=", ">", "==", "<="}, by["rel-flip"]
        assert all(site["line"] not in (1, 2, 4, 5) for site in found), "masked lines must not mutate"
        assert [site["replacement"] for site in by["off-by-one"]] == ["11", "1", "2", "4"], by["off-by-one"]
        assert {site["original"] for site in by["logic-flip"]} == {"&&", "||"}
        assert [site["line"] for site in by["negate"]] == [7, 10], "a declaration condition is not negated"
        assert [site["line"] for site in by["skip-exit"]] == [7]
        assert {site["line"] for site in by["drop-diag"]} == {14, 15}
        assert len({site["id"] for site in found}) == len(found)
        assert allocate({"a": 90, "b": 9, "c": 1}, 10) == {"a": 7, "b": 2, "c": 1}
        assert sum(allocate({"a": 3, "b": 2}, 10).values()) == 5
        assert allocate({"a": 5, "b": 5, "c": 5}, 2) in ({"a": 1, "b": 1, "c": 0}, {"a": 1, "b": 0, "c": 1},
                                                         {"a": 0, "b": 1, "c": 1})
        # End to end with a fake build (a copy into an "object") and a fake test
        # that fails when the source's behaviour changes.
        build = root / "build"
        build.mkdir()
        obj = build / "a.o"
        (build / "compile_commands.json").write_text(json.dumps(
            [{"directory": str(build), "file": str(source), "output": "a.o", "command": "cc"}]))
        build_command = [sys.executable, "-c", f"import shutil; shutil.copy({str(source)!r}, {str(obj)!r})"]
        test_command = [sys.executable, "-c",
                        f"import sys; sys.exit(0 if open({str(obj)!r}).read() == {SAMPLE_SOURCE!r} else 1)"]
        mutants, strata = sample(root, build, 4, 7)
        again, _ = sample(root, build, 4, 7)
        assert [row["id"] for row in mutants] == [row["id"] for row in again] and len(mutants) == 4
        assert strata == {"src/demo": len(found)}
        results = run_all(root, mutants[:2], build_command, [test_command], 0, 60, None)
        assert [row["status"] for row in results] == ["killed", "killed"], results
        assert source.read_text() == SAMPLE_SOURCE, "every mutant is reverted"
        survivor = [sys.executable, "-c", "pass"]
        results = run_all(root, mutants[:1], build_command, [survivor], 0, 60, None)
        assert results[0]["status"] == "survived" and summary(results)["kill_rate"] == 0.0
        broken = [sys.executable, "-c", "import sys; sys.exit(3)"]
        stale = dict(mutants[0], original="no such text")
        results = run_all(root, [stale], build_command, [survivor], 0, 60, None)
        assert results[0]["status"] == "stale"
        try:
            run_all(root, mutants[:1], broken, [survivor], 0, 60, None)
        except ValueError:
            pass
        else:
            raise AssertionError("an unbuildable baseline must stop the run")
        output = "The following tests FAILED:\n\t 12 - compiler_pipeline_frontend (Failed)\n"
        assert failing_test(output, ["ctest"]) == "compiler_pipeline_frontend"
        part = {"schema": 1, "kind": "gti-seeded-bugs", "commit": "c", "seed": 7, "requested": 4, "strata": {},
                "build_command": "b", "test_commands": ["t"]}
        with tempfile.TemporaryDirectory() as slices:
            for index, statuses in ((0, ["killed", "survived"]), (2, ["killed", "build-failed"])):
                rows = [dict(mutants[0], status=status) for status in statuses]
                Path(slices, f"{index}.json").write_text(json.dumps(dict(part, slice=[index, 3], results=rows)))
            merged = merge(sorted(Path(slices).glob("*.json")))
        assert merged["missing_slices"] == [1] and merged["summary"]["kill_rate"] == 66.67, merged["summary"]
    print("seeded_bugs self-test: ok")


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--self-test", action="store_true")
    sub = parser.add_subparsers(dest="command")
    for name in ("sample", "run"):
        command = sub.add_parser(name)
        command.add_argument("--root", default=str(Path(__file__).resolve().parents[1]))
        command.add_argument("--build", required=True, help="configured tree with compile_commands.json")
        command.add_argument("-n", type=int, default=180, help="mutants in the whole sample (before slicing)")
        command.add_argument("--seed", type=int, default=1)
        command.add_argument("--ninja-targets", help="restrict to sources these Ninja targets depend on")
        command.add_argument("--replay", help="reuse the mutants of an earlier run's JSON")
        command.add_argument("--slice", default="0/1", help="i/k: run every k-th mutant starting at i")
        command.add_argument("--output", required=True)
    run = sub.choices["run"]
    run.add_argument("--build-command", required=True, help="incremental build, split like a shell would")
    run.add_argument("--test-command", action="append", required=True,
                     help="test command (repeatable, run in order; the first failure kills the mutant)")
    run.add_argument("--time-budget-minutes", type=float, default=0, help="stop starting mutants after this")
    run.add_argument("--mutant-timeout-minutes", type=float, default=45)
    run.add_argument("--log", help="append every command's output here")
    joined = sub.add_parser("merge")
    joined.add_argument("slices", nargs="+")
    joined.add_argument("--output", required=True)
    args = parser.parse_args(argv)
    if args.self_test:
        self_test()
        return
    if args.command == "merge":
        merged = merge(args.slices)
        Path(args.output).write_text(json.dumps(merged, indent=1) + "\n")
        print("seeded bugs " + json.dumps(merged["summary"]["counts"]) + f" kill rate {merged['summary']['kill_rate']}%")
        return
    if args.command not in ("sample", "run"):
        parser.error("choose sample, run, merge or --self-test")
    root = Path(args.root).resolve()
    mutants, strata = choose(args, root)
    index, count = parse_slice(args.slice)
    mutants = mutants[index::count]
    if args.command == "sample":
        Path(args.output).write_text(json.dumps(document(args, root, mutants, strata), indent=1) + "\n")
        print(f"sampled {len(mutants)} mutants over {len(strata)} directories")
        return
    log = open(args.log, "a") if args.log else None
    try:
        results = run_all(root, mutants, shlex.split(args.build_command),
                          [shlex.split(command) for command in args.test_command],
                          args.time_budget_minutes * 60, args.mutant_timeout_minutes * 60, log)
    finally:
        if log:
            log.close()
    body = document(args, root, mutants, strata, results)
    Path(args.output).write_text(json.dumps(body, indent=1) + "\n")
    print("seeded bugs " + json.dumps(body["summary"]["counts"]) + f" kill rate {body['summary']['kill_rate']}%")


if __name__ == "__main__":
    try:
        main()
    except (ValueError, OSError, subprocess.CalledProcessError) as error:
        print("seeded_bugs: " + str(error), file=sys.stderr)
        raise SystemExit(1)
