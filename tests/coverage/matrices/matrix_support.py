"""Shared compile, run and classify support for the combination matrices.

Every matrix builds its cells with the LLVM backend, GTI's only tested
backend, at each selected optimisation level, runs them under a watchdog
and reduces the result to one verdict. A cell built at several levels whose
verdicts differ is reported as a level divergence: a program that one level
accepts and runs and another refuses or runs differently is a defect even
when no oracle says which level is right.

Verdicts:

  ok              built and, where run, exited 0 (with the expected stdout)
  ICE             internal compiler error (GTI-B0001)
  LLVM-VERIFY     the backend produced a module the LLVM verifier rejects
  UNSUPPORTED[…]  the LLVM backend declined a declared shape (GTI-B0003,
                  with the refused shape and its O-LLVM milestone)
  LINK            the native linker reported undefined symbols
  NATIVE          another native toolchain error with no GTI diagnostic
  CRASH(SIG…)     the compiler died on a signal
  SILENT          nonzero compiler exit with no output
  TIMEOUT         the compiler exceeded its time limit
  RUN(…)          the program crashed, hung or exited nonzero
  WRONG(…)        the program printed something other than expected
  S2067 etc.      an ordinary GTI diagnostic (often a correct rejection)
  DIVERGE(…)      the selected levels disagree; lists each level's verdict
  ACCEPTED(…)     a cell that must be rejected compiled
  WRONG-REJECTION a cell that must be rejected failed with another diagnostic

`hard()` is the set a matrix reports as a defect: everything except ok,
UNSUPPORTED and ordinary diagnostics. Ordinary diagnostics still need
reading; the matrix prints the code so a correct rejection can be told from
a wrong one.
"""
import argparse
import json
import os
import re
import shutil
import signal
import subprocess
import tempfile
from pathlib import Path

REPOSITORY = Path(__file__).resolve().parents[3]
LEVELS = ("O0", "O1", "O2", "O3")
REFUSAL = re.compile(r"for ((?:Shape|Mir|Hir)\w*\.\w+) \((O-LLVM-\d+)\)")
CODE = re.compile(r"error\[GTI-([A-Z][0-9]+)\]")
RUNTIME_CODE = re.compile(r"GTI-R[0-9]+")
HARD_PREFIXES = ("ICE", "LLVM-VERIFY", "LINK", "NATIVE", "CRASH", "SILENT",
                 "TIMEOUT", "RUN", "WRONG", "DIVERGE", "ACCEPTED")


def hard(verdict):
    """Whether a verdict is a defect rather than ok, a declared LLVM
    limitation or an ordinary diagnostic."""
    return verdict.startswith(HARD_PREFIXES)


def run(command, env, timeout):
    """Run a command in its own process group; kill the group on timeout."""
    process = subprocess.Popen(command, env=env, stdout=subprocess.PIPE,
                               stderr=subprocess.PIPE, text=True,
                               errors="replace", start_new_session=True)
    try:
        out, err = process.communicate(timeout=timeout)
        return process.returncode, out, err
    except subprocess.TimeoutExpired:
        try:
            os.killpg(process.pid, signal.SIGKILL)
        except (ProcessLookupError, PermissionError):
            pass
        out, err = process.communicate()
        return "timeout", out, err


def _signal(returncode):
    try:
        return signal.Signals(-returncode).name
    except ValueError:
        return f"signal{-returncode}"


def build_verdict(returncode, text):
    """Classify a failed or timed-out compiler invocation."""
    if returncode == "timeout":
        return "TIMEOUT"
    if returncode < 0:
        return f"CRASH({_signal(returncode)})"
    if "invalid LLVM module" in text:
        return "LLVM-VERIFY"
    if "GTI-B0001" in text or "internal compiler error" in text:
        return "ICE"
    refusal = REFUSAL.search(text)
    if refusal or "GTI-B0003" in text:
        return (f"UNSUPPORTED[{refusal[1]} {refusal[2]}]" if refusal
                else "UNSUPPORTED")
    if "Undefined symbols" in text or "undefined reference" in text:
        return "LINK"
    codes = CODE.findall(text)
    if codes:
        return codes[0]
    if not text.strip():
        return "SILENT"
    if "error:" in text:
        return "NATIVE"
    return f"exit{returncode}"


def run_verdict(returncode, out, expect_stdout, err="", expect_run_error=None):
    if returncode == "timeout":
        return "RUN(timeout)"
    if returncode < 0:
        return f"RUN({_signal(returncode)})"
    if expect_run_error is not None:
        # The cell must stop with this defined runtime failure.
        if returncode != 0 and re.search(expect_run_error, err):
            return "ok"
        found = RUNTIME_CODE.search(err)
        return (f"WRONG(expected failure {expect_run_error}, got "
                f"{found[0] if found else f'rc={returncode}'})")
    if returncode != 0:
        return f"RUN(rc={returncode})"
    if expect_stdout is not None and out != expect_stdout:
        return f"WRONG({out.strip()[:40]!r})"
    return "ok"


def combine(per_level):
    """One verdict for a cell from its per-level verdicts."""
    verdicts = list(per_level.values())
    if all(v == verdicts[0] for v in verdicts):
        return verdicts[0]
    return "DIVERGE(" + ",".join(f"{level}={v}"
                                 for level, v in per_level.items()) + ")"


def level_list(text):
    levels = [level.strip() for level in text.split(",") if level.strip()]
    unknown = [level for level in levels if level not in LEVELS]
    if not levels or unknown:
        raise argparse.ArgumentTypeError(
            f"levels must be a comma-separated subset of {','.join(LEVELS)}")
    return tuple(dict.fromkeys(levels))


def arguments(description, default_levels="O0", extra=None):
    """The command line every matrix shares; `extra` adds its own options."""
    parser = argparse.ArgumentParser(description=description)
    parser.add_argument("gti", type=Path, nargs="?",
                        default=REPOSITORY / "build-release" / "gti",
                        help="compiler under test (default build-release/gti)")
    parser.add_argument("--levels", type=level_list,
                        default=level_list(default_levels),
                        help=f"LLVM optimisation levels, comma-separated "
                             f"(default {default_levels})")
    parser.add_argument("--stdlib", type=Path, default=REPOSITORY / "stdlib")
    parser.add_argument("--out", type=Path,
                        help="new directory for the cells (default: a fresh "
                             "temporary directory)")
    parser.add_argument("--json", type=Path,
                        help="write every cell's per-level verdicts here")
    parser.add_argument("--compiler-flag", action="append", default=[],
                        metavar="FLAG", dest="compiler_flags",
                        help="extra compiler flag for every cell, repeatable "
                             "(for example --compiler-flag=-g, so that a run "
                             "compares with a --json report made without it)")
    parser.add_argument("--baseline", type=Path,
                        help="an earlier --json report; print every cell "
                             "whose verdict changed")
    if extra:
        extra(parser)
    return parser.parse_args()


class Matrix:
    """A run of one matrix: its work directory, environment and results."""

    def __init__(self, description, name, default_levels="O0", extra=None):
        self.args = arguments(description, default_levels, extra)
        self.name = name
        self.gti = self.args.gti.resolve()
        if not self.gti.is_file():
            raise SystemExit(f"{name}: compiler not found: {self.gti}")
        self.levels = self.args.levels
        if self.args.out:
            self.out = self.args.out.resolve()
            if self.out.exists() and any(self.out.iterdir()):
                raise SystemExit(f"{name}: --out must be new or empty: {self.out}")
            self.out.mkdir(parents=True, exist_ok=True)
        else:
            self.out = Path(tempfile.mkdtemp(prefix=f"gti-{name}-"))
        self.env = dict(os.environ, GTI_STDLIB_PATH=str(self.args.stdlib.resolve()),
                        NO_COLOR="1")
        # Failed builds save backend evidence under TMPDIR; keep it beside the
        # cells so a run cleans up after itself.
        self.scratch = self.out / ".tmp"
        self.scratch.mkdir(exist_ok=True)
        self.env["TMPDIR"] = str(self.scratch)
        self.records = {}

    def level(self, source, level, *, execute=True, flags=(),
              expect_stdout=None, expect_build_error=None,
              expect_run_error=None, compile_timeout=600, run_timeout=60):
        """Verdict for one cell at one LLVM level.

        `expect_build_error` is a regular expression the compiler output must
        match for a cell that must be rejected; `expect_run_error` one the
        program's stderr must match for a cell that must stop with a defined
        runtime failure."""
        executable = source.with_name(f"{source.stem}-{level}")
        try:
            returncode, out, err = run(
                [str(self.gti), str(source), "--backend", "llvm", f"-{level}",
                 *flags, *self.args.compiler_flags, "-o", str(executable)],
                self.env, compile_timeout)
            if expect_build_error is not None:
                if returncode == 0:
                    return "ACCEPTED(expected rejection)"
                verdict = build_verdict(returncode, out + err)
                if hard(verdict) or not re.search(expect_build_error, out + err):
                    return verdict if hard(verdict) else f"WRONG-REJECTION({verdict})"
                return "ok"
            if returncode != 0 or not executable.exists():
                if returncode == 0:
                    return "NATIVE"
                return build_verdict(returncode, out + err)
            if not execute:
                return "ok"
            returncode, out, err = run([str(executable)], self.env, run_timeout)
            return run_verdict(returncode, out, expect_stdout, err,
                               expect_run_error)
        finally:
            # Cell executables are large; keep only the sources.
            for artifact in source.parent.glob(f"{executable.name}*"):
                if artifact.is_dir():
                    shutil.rmtree(artifact, ignore_errors=True)
                else:
                    artifact.unlink(missing_ok=True)

    def classify(self, source, *, key=None, **options):
        """Build (and by default run) a cell at every selected level and
        return its combined verdict."""
        per_level = {level: self.level(source, level, **options)
                     for level in self.levels}
        verdict = combine(per_level)
        self.records[key or str(source.relative_to(self.out))] = {
            "levels": per_level, "verdict": verdict}
        return verdict

    def finish(self, failures):
        """Write the JSON report, clean up and return the exit status."""
        shutil.rmtree(self.scratch, ignore_errors=True)
        if self.args.json:
            self.args.json.write_text(json.dumps({
                "schema": 1, "matrix": self.name, "compiler": str(self.gti),
                "compiler_flags": self.args.compiler_flags,
                "levels": list(self.levels), "cells": self.records,
                "hard": len(failures)}, indent=2, sort_keys=True) + "\n",
                encoding="utf-8")
        if self.args.baseline:
            self.compare(json.loads(self.args.baseline.read_text(encoding="utf-8")))
        print(f"sources in {self.out}")
        return 1 if failures else 0

    def compare(self, baseline):
        """Print cells whose verdict differs from an earlier report."""
        before = {cell: record["verdict"]
                  for cell, record in baseline.get("cells", {}).items()}
        changed = [(cell, before.get(cell, "absent"), record["verdict"])
                   for cell, record in sorted(self.records.items())
                   if before.get(cell, "absent") != record["verdict"]]
        print(f"against baseline: {len(changed)} changed cells")
        for cell, old, new in changed:
            print(f"  {cell}: {old} -> {new}")
