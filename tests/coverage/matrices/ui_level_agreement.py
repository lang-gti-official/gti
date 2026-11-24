#!/usr/bin/env python3
"""Every passing UI test builds, and runs alike, at every LLVM level.

The per-commit suite builds most `tests/ui` programs once, at O0 (or the
level a test's `//@ opt:` names). This sweep takes each single-file
`//@ check-pass` and `//@ run-pass` test and builds it at each selected
level with the test's own execution profile. A `run-pass` test is also run.
All levels must agree: the same build verdict and, for `run-pass`, the same
exit status and standard output. This is the RFC 0013 level-agreement oracle
applied to the project's own hand-written corpus, the largest body of valid
programs GTI has; it needs no expected values.

Skipped: multi-file tests (`// --- name` sections), `//@ revisions`, and
tests with any other kind (fail, analyze, parse and format tests).
"""
import concurrent.futures
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import matrix_support  # noqa: E402

DIRECTIVE = re.compile(r"^//@ ([a-z-]+)(?::\s*(.*))?$", re.MULTILINE)


def select(root, pattern):
    tests = []
    for path in sorted(root.rglob("*.gti")):
        if pattern and not re.search(pattern, str(path.relative_to(root))):
            continue
        text = path.read_text(encoding="utf-8", errors="replace")
        if "\n// --- " in text or text.startswith("// --- "):
            continue
        directives = {name: value for name, value in DIRECTIVE.findall(text)}
        kinds = {"check-pass", "run-pass"} & set(directives)
        if len(kinds) != 1 or "revisions" in directives:
            continue
        flags = []
        if directives.get("execution-profile"):
            flags += ["--execution-profile", directives["execution-profile"].strip()]
        tests.append((path, kinds.pop(), tuple(flags)))
    return tests


def options(parser):
    parser.add_argument("--filter", help="regular expression on the path under tests/ui")
    parser.add_argument("--jobs", type=int, default=4)
    parser.add_argument("--run-timeout", type=int, default=30)


def main():
    matrix = matrix_support.Matrix(__doc__.split("\n\n")[0], "ui-level-agreement",
                                   default_levels="O0,O1,O2,O3", extra=options)
    root = matrix_support.REPOSITORY / "tests" / "ui"
    tests = select(root, matrix.args.filter)

    def observe(item):
        path, kind, flags = item
        key = str(path.relative_to(root))
        per_level = {}
        for level in matrix.levels:
            executable = matrix.out / (key.replace("/", "__") + f"-{level}")
            try:
                returncode, out, err = matrix_support.run(
                    [str(matrix.gti), str(path), "--backend", "llvm", f"-{level}",
                     *flags, *matrix.args.compiler_flags, "-o", str(executable)],
                    matrix.env, 600)
                if returncode != 0 or not executable.exists():
                    per_level[level] = ("NATIVE" if returncode == 0
                                        else matrix_support.build_verdict(returncode, out + err))
                    continue
                if kind == "check-pass":
                    per_level[level] = "built"
                    continue
                returncode, out, _ = matrix_support.run(
                    [str(executable)], matrix.env, matrix.args.run_timeout)
                per_level[level] = (f"run(rc={returncode},stdout={out.strip()[:40]!r})"
                                    if returncode != "timeout" else "RUN(timeout)")
            finally:
                for artifact in matrix.out.glob(executable.name + "*"):
                    if artifact.is_file():
                        artifact.unlink(missing_ok=True)
        verdict = matrix_support.combine(per_level)
        if not verdict.startswith("DIVERGE") and verdict not in ("built",) \
                and not verdict.startswith("run("):
            verdict = f"FAILS-EVERYWHERE({verdict})"
        matrix.records[key] = {"levels": per_level, "verdict": verdict}
        return key, verdict

    failures = []
    with concurrent.futures.ThreadPoolExecutor(matrix.args.jobs) as pool:
        for key, verdict in pool.map(observe, tests):
            if verdict.startswith(("DIVERGE", "FAILS")):
                failures.append((key, verdict))
                print(f"{key:70} {verdict}", flush=True)
    print(f"{len(tests)} UI tests at {','.join(matrix.levels)}: "
          f"{len(failures)} disagree or fail at every level")
    return matrix.finish(failures)


if __name__ == "__main__":
    sys.exit(main())
