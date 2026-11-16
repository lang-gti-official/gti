#!/usr/bin/env python3
"""Write one tests/ui case converted from an old Python harness and prove it.

Conversion aid for the test-suite redesign (docs/plans/test-suite-redesign.md,
phase 3); it is deleted with the old suite. Each call writes one `.gti` file
with a purpose line and directives, blesses only the annotations of a
check-fail case (the code and line of every error), and then runs the case
unblessed, failing when an expected diagnostic code is missing or the case
does not pass.

    scripts/ui_convert.py tests/ui/area/name.gti --desc "What it proves." \
        --from tests/fixtures/x.gti --mode run-pass --stdout 3 --stdout 4
    scripts/ui_convert.py tests/ui/area/bad.gti --desc "..." --from x.gti \
        --replace "return 0;" "missing(); return 0;" --expect GTI-S2001
"""

from __future__ import annotations

import argparse
import os
import re
import subprocess
import sys
from pathlib import Path

REPOSITORY = Path(__file__).resolve().parent.parent


def runner(build: Path) -> Path:
    return build / "gti-test"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("dest", type=Path)
    parser.add_argument("--desc", required=True)
    source = parser.add_mutually_exclusive_group(required=True)
    source.add_argument("--from", dest="source", type=Path)
    source.add_argument("--text")
    parser.add_argument("--mode", choices=("check-pass", "check-fail", "build-pass",
                                           "run-pass", "run-fail"))
    parser.add_argument("--replace", nargs=2, action="append", default=[],
                        metavar=("OLD", "NEW"), help="exact, must occur")
    parser.add_argument("--stdout", action="append", default=[])
    parser.add_argument("--bless-stdout", action="store_true",
                        help="record whatever the program prints")
    parser.add_argument("--exit-code", type=int)
    parser.add_argument("--stderr-contains", action="append", default=[])
    parser.add_argument("--args")
    parser.add_argument("--stdin", action="append", default=[])
    parser.add_argument("--directive", action="append", default=[],
                        help="extra raw directive text after //@ ")
    parser.add_argument("--expect", action="append", default=[],
                        help="diagnostic code a check-fail case must report")
    parser.add_argument("--message", action="append", default=[], metavar="CODE=TEXT",
                        help="add a message substring to that code's blessed annotation")
    parser.add_argument("--build", type=Path, default=REPOSITORY / "build-release")
    parser.add_argument("--force", action="store_true")
    parser.add_argument("--section", action="store_true",
                        help="put the program in a `// --- main.gti` section so its "
                             "line numbers stay those of the source")
    arguments = parser.parse_args()

    dest = arguments.dest if arguments.dest.is_absolute() else REPOSITORY / arguments.dest
    if dest.exists() and not arguments.force:
        print(f"exists: {dest}", file=sys.stderr)
        return 2
    text = arguments.source.read_text() if arguments.source else arguments.text
    original = text
    for old, new in arguments.replace:
        if old not in text:
            print(f"replacement text not found: {old!r}", file=sys.stderr)
            return 2
        text = text.replace(old, new)
    mode = arguments.mode or ("check-fail" if arguments.expect else "check-pass")
    header = [f"// {arguments.desc}", f"//@ {mode}"]
    header += [f"//@ {d}" for d in arguments.directive]
    if arguments.exit_code is not None:
        header.append(f"//@ exit-code: {arguments.exit_code}")
    header += [f"//@ stderr-contains: {s}" for s in arguments.stderr_contains]
    header += [f"//@ stdout: {s}" for s in arguments.stdout]
    if arguments.args:
        header.append(f"//@ args: {arguments.args}")
    header += [f"//@ stdin: {s}" for s in arguments.stdin]
    # Annotations from the source program are replaced by blessed ones.
    body = text if text.endswith("\n") else text + "\n"
    if mode == "check-fail" and "//~" not in body:
        body += "//~? ERROR GTI-X0000 placeholder replaced by bless\n"
    # `{line:TEXT}` names the file line of the first body line containing
    # TEXT (`{line:TEXT>AFTER}`: the first one after a line containing AFTER),
    # for runtime errors that report `main.gti:<line>`.
    body_lines = body.splitlines()
    if len(original.splitlines()) == len(body_lines):
        body_lines = original.splitlines()  # line sites named before replacement

    def line_of(match: re.Match[str]) -> str:
        needle, _, after = match.group(1).partition(">")
        start = 0
        if after:
            start = next(i for i, line in enumerate(body_lines) if after in line) + 1
        index = next(i for i in range(start, len(body_lines)) if needle in body_lines[i])
        return str(offset + index + 1)

    offset = 0 if arguments.section else len(header)
    header = [re.sub(r"\{line:(.*?)\}(?=:|\s|$)", line_of, line) for line in header]
    # `{src:N}` is line N of the source program, shifted below the header.
    header = [re.sub(r"\{src:(\d+)\}", lambda m: str(offset + int(m.group(1))), line)
              for line in header]
    dest.parent.mkdir(parents=True, exist_ok=True)
    if arguments.section:
        header.append("// --- main.gti")
    dest.write_text("\n".join(header) + "\n" + body)

    gti_test = runner(arguments.build)
    environment = {k: v for k, v in os.environ.items() if k != "CI"}
    # Bless records every diagnostic (warnings included); it may not change
    # the run expectations this conversion declared.
    blessed = subprocess.run([str(gti_test), str(dest), "--bless", "--jobs", "1"],
                             capture_output=True, text=True, env=environment)
    if "ICE" in blessed.stdout.split("RESULT")[0]:
        print(blessed.stdout, file=sys.stderr)
        return 1
    stdout_file = dest.with_suffix(".stdout")
    declared = [line for line in dest.read_text().splitlines() if line.startswith("//@ stdout:")]
    if not arguments.bless_stdout and (
            declared != [f"//@ stdout: {s}" for s in arguments.stdout] or stdout_file.exists()):
        print(f"stdout differs from the declared expectation: {declared}", file=sys.stderr)
        print(blessed.stdout, file=sys.stderr)
        return 1
    for item in arguments.message:
        code, _, message = item.partition("=")
        written = dest.read_text()
        annotation = re.search(r"//~[\^v|?]*\s+ERROR\s+" + re.escape(code) + r"\b", written)
        if not annotation:
            print(f"no {code} annotation for its message", file=sys.stderr)
            return 1
        dest.write_text(written[:annotation.end()] + " " + message + written[annotation.end():])
    checked = subprocess.run([str(gti_test), str(dest), "--jobs", "1"],
                             capture_output=True, text=True, env=environment)
    result = checked.stdout.strip().splitlines()[-1] if checked.stdout.strip() else ""
    written = dest.read_text()
    missing = [code for code in arguments.expect
               if not re.search(r"//~[\^v|?]*\s+ERROR\s+" + re.escape(code) + r"\b", written)]
    if checked.returncode != 0 or missing:
        print("\n".join((checked.stdout + checked.stderr).splitlines()[:8]), file=sys.stderr)
        if missing:
            print(f"missing expected codes {missing} in {dest}", file=sys.stderr)
        return 1
    codes = re.findall(r"//~[\^v|?]*\s+(?:ERROR|WARN)\s+(GTI-\w+)", written)
    cpu = re.search(r"([0-9.]+) cpu-s", result)
    print(f"ok {dest.relative_to(REPOSITORY)} {mode} cpu={cpu.group(1) if cpu else '?'}"
          + (f" codes={','.join(codes)}" if codes else ""))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
