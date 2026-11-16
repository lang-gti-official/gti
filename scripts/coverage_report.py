#!/usr/bin/env python3
"""Collect and summarize Clang source-based coverage of the GTI suite.

The suite-measures workflow builds with -DGTI_ENABLE_SOURCE_COVERAGE=ON and
runs every shard with LLVM_PROFILE_FILE=<dir>/%m.profraw, so concurrent
processes of one binary merge into one raw profile. Each shard then runs

    coverage_report.py collect --build build --profiles <dir> --output shard.lcov.gz

which merges that shard's raw profiles and exports line and branch records for
first-party sources (repository-relative `src/` and `runtime/` paths) as LCOV.
LCOV from separate shards of the same commit unions exactly, so a final job runs

    coverage_report.py summarize --commit <sha> --output coverage.json shard-*.lcov.gz

to produce the compact per-file summary that scripts/coverage_compare.py reads.
`--self-test` checks the LCOV merge and summary arithmetic without LLVM.
"""

import argparse
import datetime
import gzip
import io
import json
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

SCHEMA = 1
SCOPES = ("src/", "runtime/")
# Section names Clang's coverage mapping occupies (ELF and Mach-O spellings).
COVERAGE_SECTIONS = ("__llvm_covfun", "__llvm_covmap")
BINARY_MAGIC = (b"\x7fELF", b"\xcf\xfa\xed\xfe")


def tool(name, llvm_bin):
    directory = llvm_bin or os.environ.get("GTI_COVERAGE_LLVM_BIN", "")
    if directory:
        candidate = Path(directory) / name
        if candidate.is_file():
            return str(candidate)
        raise ValueError(f"{candidate} does not exist")
    found = shutil.which(name)
    if not found:
        raise ValueError(f"{name} is not on PATH; pass --llvm-bin")
    return found


def instrumented_binaries(build, objdump):
    """Executables and shared objects in the build tree carrying a coverage map."""
    found = []
    for path in sorted(Path(build).rglob("*")):
        if not path.is_file() or path.is_symlink() or path.suffix in (".o", ".a", ".obj"):
            continue
        if "CMakeFiles" in path.parts:
            continue
        try:
            with path.open("rb") as handle:
                if handle.read(4) not in BINARY_MAGIC:
                    continue
        except OSError:
            continue
        headers = subprocess.run([objdump, "--section-headers", str(path)],
                                 capture_output=True, text=True)
        if headers.returncode == 0 and any(name in headers.stdout for name in COVERAGE_SECTIONS):
            found.append(path)
    return found


def relative_source(path, root):
    """The repository-relative path of an in-scope source, or None."""
    try:
        relative = Path(path).resolve().relative_to(root).as_posix()
    except (ValueError, OSError):
        return None
    return relative if relative.startswith(SCOPES) else None


def filter_lcov(text, root):
    """Keep only in-scope records, rewriting SF: to repository-relative paths."""
    kept, record, keep = [], [], False
    for line in text.splitlines():
        if line.startswith("SF:"):
            relative = relative_source(line[3:], root)
            keep, record = relative is not None, ["SF:" + (relative or "")]
            continue
        record.append(line)
        if line == "end_of_record":
            if keep:
                kept.extend(record)
            record, keep = [], False
    return "\n".join(kept) + ("\n" if kept else "")


def collect(build, profiles, root, output, llvm_bin=None, jobs=0):
    raw = sorted(Path(profiles).rglob("*.profraw"))
    if not raw:
        raise ValueError(f"no .profraw files under {profiles}")
    profdata, cov, objdump = (tool(name, llvm_bin) for name in ("llvm-profdata", "llvm-cov", "llvm-objdump"))
    binaries = instrumented_binaries(build, objdump)
    if not binaries:
        raise ValueError(f"no coverage-instrumented binaries under {build}")
    with tempfile.TemporaryDirectory(prefix="gti-coverage-") as directory:
        listing = Path(directory) / "inputs.txt"
        listing.write_text("".join(f"{path}\n" for path in raw))
        merged = Path(directory) / "merged.profdata"
        # A process killed while writing leaves a truncated profile; drop it
        # rather than the shard (`all` fails only when every input fails).
        subprocess.run([profdata, "merge", "-sparse", "--failure-mode=all", f"--num-threads={jobs}",
                        f"--input-files={listing}", "-o", str(merged)], check=True)
        command = [cov, "export", "-format=lcov", f"-instr-profile={merged}", str(binaries[0])]
        for binary in binaries[1:]:
            command += ["-object", str(binary)]
        command += ["-ignore-filename-regex=^/usr/", f"-ignore-filename-regex=^{Path(build).resolve()}/"]
        exported = subprocess.run(command, check=True, capture_output=True, text=True).stdout
    filtered = filter_lcov(exported, Path(root).resolve())
    Path(output).parent.mkdir(parents=True, exist_ok=True)
    with gzip.open(output, "wt") as handle:
        handle.write(filtered)
    files = filtered.count("end_of_record")
    print(f"coverage collect: {len(raw)} raw profiles, {len(binaries)} binaries, {files} source files -> {output}")


def read_lcov(path):
    opener = gzip.open if str(path).endswith(".gz") else open
    with opener(path, "rt") as handle:
        return handle.read()


def merge_lcov(texts):
    """{file: {"lines": {line: count}, "branches": {(line, block, branch): count}}}"""
    files = {}
    for text in texts:
        current = None
        for line in io.StringIO(text):
            line = line.rstrip("\n")
            if line.startswith("SF:"):
                current = files.setdefault(line[3:], {"lines": {}, "branches": {}})
            elif current is None:
                continue
            elif line.startswith("DA:"):
                number, count = line[3:].split(",")[:2]
                current["lines"][int(number)] = current["lines"].get(int(number), 0) + int(count)
            elif line.startswith("BRDA:"):
                number, block, branch, taken = line[5:].split(",")
                key = (int(number), block, branch)
                current["branches"][key] = current["branches"].get(key, 0) + (0 if taken == "-" else int(taken))
            elif line == "end_of_record":
                current = None
    return files


def percent(covered, total):
    return round(100.0 * covered / total, 2) if total else None


def summarize(files, commit, meta=None):
    rows, totals, directories = {}, {"lines": [0, 0], "branches": [0, 0]}, {}
    for name in sorted(files):
        lines, branches = files[name]["lines"], files[name]["branches"]
        row = {"lines": [sum(1 for count in lines.values() if count > 0), len(lines)],
               "branches": [sum(1 for count in branches.values() if count > 0), len(branches)]}
        rows[name] = row
        directory = directories.setdefault(str(Path(name).parent), {"lines": [0, 0], "branches": [0, 0]})
        for key in ("lines", "branches"):
            for bucket in (totals, directory):
                bucket[key][0] += row[key][0]
                bucket[key][1] += row[key][1]
    for bucket in (totals, *directories.values()):
        bucket["line_percent"] = percent(*bucket["lines"])
        bucket["branch_percent"] = percent(*bucket["branches"])
    return {"schema": SCHEMA, "kind": "gti-source-coverage", "commit": commit,
            "generated_at": datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
            "meta": meta or {}, "units": "[covered, total] instrumented lines and branches",
            "totals": totals, "directories": dict(sorted(directories.items())), "files": rows}


def dump(document):
    """Indented JSON with one line per file, so diffs and grep stay readable."""
    files = document["files"]
    head = json.dumps({key: value for key, value in document.items() if key != "files"}, indent=1)
    body = ",\n".join(f"  {json.dumps(name)}: {json.dumps(row, separators=(',', ':'))}" for name, row in files.items())
    return head[:-2] + ',\n "files": {\n' + body + "\n }\n}\n"


def write_lcov(files, output):
    with gzip.open(output, "wt") as handle:
        for name in sorted(files):
            handle.write(f"SF:{name}\n")
            for (number, block, branch), count in sorted(files[name]["branches"].items()):
                handle.write(f"BRDA:{number},{block},{branch},{count}\n")
            for number, count in sorted(files[name]["lines"].items()):
                handle.write(f"DA:{number},{count}\n")
            handle.write("end_of_record\n")


def self_test():
    root = Path("/repo")
    exported = ("SF:/repo/src/a.cpp\nDA:1,0\nDA:2,3\nBRDA:2,0,0,1\nBRDA:2,0,1,0\nend_of_record\n"
                "SF:/usr/include/x.h\nDA:1,1\nend_of_record\n"
                "SF:/repo/tests/t.cpp\nDA:1,1\nend_of_record\n"
                "SF:/repo/runtime/src/r.cpp\nDA:5,0\nBRDA:5,0,0,-\nend_of_record\n")
    filtered = filter_lcov(exported, root)
    assert "SF:src/a.cpp" in filtered and "SF:runtime/src/r.cpp" in filtered, filtered
    assert "/usr/" not in filtered and "tests/" not in filtered, filtered
    other = "SF:src/a.cpp\nDA:1,2\nDA:2,0\nBRDA:2,0,1,4\nend_of_record\nSF:src/b.cpp\nDA:9,0\nend_of_record\n"
    files = merge_lcov([filtered, other])
    document = summarize(files, "abc", {"shards": 2})
    assert document["files"]["src/a.cpp"] == {"lines": [2, 2], "branches": [2, 2]}, document["files"]
    assert document["files"]["src/b.cpp"] == {"lines": [0, 1], "branches": [0, 0]}
    assert document["files"]["runtime/src/r.cpp"] == {"lines": [0, 1], "branches": [0, 1]}
    assert document["totals"]["lines"] == [2, 4] and document["totals"]["line_percent"] == 50.0
    assert document["directories"]["src"]["lines"] == [2, 3]
    assert json.loads(dump(document)) == document
    with tempfile.TemporaryDirectory() as directory:
        path = Path(directory) / "merged.lcov.gz"
        write_lcov(files, path)
        assert merge_lcov([read_lcov(path)]) == files
    print("coverage_report self-test: ok")


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--self-test", action="store_true")
    sub = parser.add_subparsers(dest="command")
    gather = sub.add_parser("collect", help="merge one shard's raw profiles and export in-scope LCOV")
    gather.add_argument("--build", required=True)
    gather.add_argument("--profiles", required=True)
    gather.add_argument("--root", default=str(Path(__file__).resolve().parents[1]))
    gather.add_argument("--output", required=True)
    gather.add_argument("--llvm-bin", help="directory of llvm-profdata/llvm-cov/llvm-objdump (default $GTI_COVERAGE_LLVM_BIN or PATH)")
    gather.add_argument("--jobs", type=int, default=0)
    summary = sub.add_parser("summarize", help="union shard LCOV files into the per-file JSON summary")
    summary.add_argument("lcov", nargs="+")
    summary.add_argument("--commit", required=True)
    summary.add_argument("--output", required=True)
    summary.add_argument("--lcov-output", help="also write the merged LCOV (gzip)")
    summary.add_argument("--meta", action="append", default=[], help="key=value recorded under meta")
    args = parser.parse_args(argv)
    if args.self_test:
        self_test()
        return
    if args.command == "collect":
        collect(args.build, args.profiles, args.root, args.output, args.llvm_bin, args.jobs)
    elif args.command == "summarize":
        files = merge_lcov(read_lcov(path) for path in args.lcov)
        if not files:
            raise ValueError("no in-scope coverage records")
        meta = dict(item.split("=", 1) for item in args.meta)
        meta["lcov_inputs"] = len(args.lcov)
        document = summarize(files, args.commit, meta)
        Path(args.output).write_text(dump(document))
        if args.lcov_output:
            write_lcov(files, args.lcov_output)
        totals = document["totals"]
        print(f"coverage {args.commit}: {len(files)} files, lines {totals['line_percent']}% "
              f"({totals['lines'][0]}/{totals['lines'][1]}), branches {totals['branch_percent']}% "
              f"({totals['branches'][0]}/{totals['branches'][1]})")
    else:
        parser.error("choose collect, summarize or --self-test")


if __name__ == "__main__":
    try:
        main()
    except (ValueError, OSError, subprocess.CalledProcessError) as error:
        print("coverage_report: " + str(error), file=sys.stderr)
        raise SystemExit(1)
