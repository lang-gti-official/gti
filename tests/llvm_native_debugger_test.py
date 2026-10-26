#!/usr/bin/env python3
"""Native LLVM source breakpoint, locals, step and backtrace acceptance probe.

Requires the composed production debug hooks. Unsupported emission is a failed
gate; host debugger permission denial is retained as blocked, never a pass.
No C++ source mapping or debugger expression-language plugin is used.
"""

import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess
import sys

from source_debug_info_test import debugger


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--gti", required=True, type=Path)
    parser.add_argument("--gti-build-cache", required=True, type=Path)
    parser.add_argument("--lldb", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--optimization", choices=(0, 3), type=int, default=0)
    parser.add_argument("--case", choices=("basic", "lexical", "inlined", "receiver", "masked", "pack", "owned-pack", "failure", "lambda", "callback", "callback-failure"), default="basic")
    parser.add_argument("--cc", default="clang")
    args = parser.parse_args()
    failure = args.case in ("failure", "callback-failure")
    callback = args.case in ("callback", "callback-failure")
    failure_line = 8 if callback else 5
    assert args.case not in ("inlined", "masked") or args.optimization == 3
    root = Path(__file__).resolve().parents[1]
    output = args.output.resolve()
    assert not output.exists() and not output.is_relative_to(root)
    assert args.gti.resolve().parent == args.gti_build_cache.resolve().parent
    assert re.search(r"^CMAKE_BUILD_TYPE:STRING=(Release|RelWithDebInfo|MinSizeRel)$",
                     args.gti_build_cache.read_text(), re.M)
    output.mkdir(parents=True)
    source = output / "native_debug.gti"
    fixture = "native_debug_optimized.gti" if args.optimization == 3 else "native_debug.gti"
    if args.case == "lexical":
        fixture = ("native_debug_lexical_optimized.gti" if args.optimization == 3
                   else "native_debug_lexical.gti")
    elif args.case == "inlined":
        fixture = "native_debug_inline.gti"
    elif args.case == "receiver":
        fixture = "native_debug_receiver.gti"
    elif args.case == "masked":
        fixture = "native_debug_masked.gti"
    elif args.case == "pack":
        fixture = "native_debug_pack.gti"
    elif args.case == "owned-pack":
        fixture = "native_debug_owned_pack.gti"
    elif args.case == "failure":
        fixture = "native_debug_failure.gti"
    elif args.case == "lambda":
        fixture = "native_debug_lambda.gti"
    elif callback:
        fixture = ("native_debug_callback_failure.gti" if failure
                   else "native_debug_callback.gti")
    shutil.copyfile(root / "tests/fixtures/llvm_optimization" / fixture, source)
    manifest = {
        "status": "running", "commands": [],
        "gti_sha256": hashlib.sha256(args.gti.read_bytes()).hexdigest(),
        "source_sha256": hashlib.sha256(source.read_bytes()).hexdigest(),
        "optimization": args.optimization,
        "case": args.case,
        "pending": ["broader optimized aggregate and borrowed variable availability",
                    "defined-failure cleanup and native callback backtraces",
                    "complete two-host source-exact matrix"],
    }

    def run(argv, expected=0):
        result = subprocess.run([str(x) for x in argv], cwd=output,
                                capture_output=True, text=True, timeout=90)
        log = f"command-{len(manifest['commands']):03}.log"
        (output / log).write_text(result.stdout + result.stderr)
        manifest["commands"].append({"argv": [str(x) for x in argv],
                                     "exit": result.returncode, "log": log})
        assert result.returncode == expected, str(output / log)
        return result

    try:
        manifest["gti_version"] = run([args.gti, "--version"]).stdout.strip()
        manifest["lldb_version"] = run([args.lldb, "--version"]).stdout.strip()
        binary = output / "native-debug"
        native = []
        if args.optimization == 3 or callback or args.case in ("receiver", "pack", "owned-pack", "failure", "lambda"):
            provider = root / "tests/fixtures/llvm_optimization" / (
                "native_debug_callback.c" if callback else "native_debug_probe.c")
            manifest["native_probe_sha256"] = hashlib.sha256(provider.read_bytes()).hexdigest()
            manifest["cc_version"] = run([args.cc, "--version"]).stdout.strip()
            obj = output / "debug-probe.o"
            run([args.cc, "-std=c17", "-g", "-O3", "-c", provider, "-o", obj])
            native = ["--", obj]
        run([args.gti, source, "--backend", "llvm", "-g", f"-O{args.optimization}",
             "--progress", "never", "-o", binary, *native])
        result = run([binary], expected=70 if failure else 0)
        if failure:
            assert 'GTI-R0002' in result.stderr and f'native_debug.gti:{failure_line}' in result.stderr
            assert result.stdout == ("D" if callback else "")
        else:
            assert not result.stdout and not result.stderr
        manifest["binary_sha256"] = hashlib.sha256(binary.read_bytes()).hexdigest()
        if sys.platform == "darwin":
            symbols = Path(str(binary) + ".dSYM") / "Contents/Resources/DWARF" / binary.name
            assert symbols.is_file(), "Darwin production debug companion is missing"
        line = (8 if callback else 6 if args.case == "lambda" else 5 if failure else 4 if args.case in ("pack", "owned-pack") else 7 if args.case == "receiver" else 6 if args.case == "lexical"
                else 5 if args.optimization == 3 else 4)
        command = [str(args.lldb), "--no-lldbinit", "--batch", str(binary),
                   "-o", f"breakpoint set --file native_debug.gti --line {line}",
                   "-o", "run", "-o", "frame info",
                   "-o", "frame variable -- input " + ("rest" if args.case in ("pack", "owned-pack") else "local") +
                   (" this" if args.case == "receiver" else ""),
                   "-o", "thread backtrace"]
        if args.case == "lexical":
            command += ["-o", "breakpoint set --file native_debug.gti --line 8",
                        "-o", "continue", "-o", "frame info",
                        "-o", "frame variable -- input local"]
        elif args.optimization == 0 and not callback and args.case not in ("receiver", "pack", "owned-pack", "failure", "lambda"):
            command += ["-o", "thread step-over", "-o", "frame info",
                        "-o", "frame variable -- input local"]
        command += ["-o", "continue"]
        result = debugger(command, output)
        transcript = result.stdout + result.stderr
        (output / "lldb.log").write_text(transcript)
        manifest["commands"].append({"argv": command, "exit": result.returncode,
                                     "log": "lldb.log"})
        denied = ("personality set failed", "Operation not permitted", "Not allowed to attach",
                  "Developer mode is disabled", "cannot get permission to debug processes")
        if any(reason in transcript for reason in denied):
            manifest["status"] = "blocked-host-debugger-permission"
            print(manifest["status"])
            return 77
        assert result.returncode == 0, str(output / "lldb.log")
        assert "stop reason = breakpoint" in transcript
        assert re.search(rf"native_debug\.gti:{line}\b", transcript)
        if failure:
            assert re.search(r"\binput = 0\b", transcript), transcript
            assert re.search(r"\blocal = 9\b", transcript), transcript
            assert 'GTI-R0002' in transcript and f'native_debug.gti:{failure_line}' in transcript
        elif args.case == "lambda" or callback:
            assert re.search(r"\binput = 4\b", transcript), transcript
            assert re.search(r"\blocal = 4\b", transcript), transcript
        elif args.case == "owned-pack":
            assert re.search(r"\binput = 4\b", transcript), transcript
            assert re.search(r"\brest =", transcript), transcript
            assert re.search(r"\[0\] =.*?value = 9\b", transcript, re.S), transcript
            assert re.search(r"\[1\] =.*?value = 9\b", transcript, re.S), transcript
        elif args.case == "pack":
            assert re.search(r"\binput = 4\b", transcript), transcript
            assert re.search(r"\brest =", transcript), transcript
            assert re.search(r"\[0\] = 9\b", transcript), transcript
            assert re.search(r"\[1\] = 11\b", transcript), transcript
        elif args.case == "receiver":
            for name in ("input", "local", "value"):
                assert re.search(rf"\b{name} = 4\b", transcript), transcript
            assert re.search(r"\bthis =", transcript), transcript
        elif args.case == "lexical":
            assert re.search(r"native_debug\.gti:8\b", transcript)
            values = re.findall(r"\blocal = (\d+)\b", transcript)
            assert values == ["11", "9"], transcript
            assert re.search(r"\binput = 4\b", transcript)
        elif args.optimization == 0:
            assert re.search(r"native_debug\.gti:5\b", transcript)
            assert re.search(r"\binput = 4\b", transcript)
            assert re.search(r"\blocal = 9\b", transcript)
            assert re.search(r"\blocal = 10\b", transcript)
        else:
            for name in ("input", "local"):
                assert re.search(rf"\b{name} = 4\b", transcript), transcript
            assert re.search(r"frame #\d+.*(?:\[inlined\].*inspect|inspect.*\[inlined\])", transcript), transcript
        assert re.search(r"frame #\d+.*" + (r"<lambda>" if args.case == "lambda" else "inspect"), transcript)
        assert re.search(r"frame #\d+.*main", transcript)
        if callback:
            assert re.search(r"frame #\d+.*debug_invoke", transcript), transcript
        assert f"exited with status = {70 if failure else 0}" in transcript
        manifest["status"] = f"passed-native-debugger-{args.case}-O{args.optimization}"
        print(manifest["status"] + "; remaining debug matrix stays explicit")
    except BaseException as error:
        manifest["status"], manifest["error"] = "failed", str(error)
        raise
    finally:
        (output / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
