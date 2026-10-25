#!/usr/bin/env python3
"""Production visibility/failure/cleanup checks, with explicit callable selection.

The foreign provider selects an unknown target at runtime. A pointer may hold a
fallible or infallible target of the same signature; no failure ABI change is
permitted. Both cases require native-C scalar declaration admission; direct calls
need no O09 implementation. Admission failures remain failed gates, not skips.
"""

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--gti", required=True, type=Path)
    parser.add_argument("--gti-build-cache", required=True, type=Path)
    parser.add_argument("--clang", required=True, type=Path)
    parser.add_argument("--case", choices=("direct", "callable"), required=True)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    fixtures = root / "tests/fixtures/llvm_optimization"
    output = args.output.resolve()
    assert not output.exists() and not output.is_relative_to(root)
    assert args.gti.resolve().parent == args.gti_build_cache.resolve().parent
    assert re.search(r"^CMAKE_BUILD_TYPE:STRING=(Release|RelWithDebInfo|MinSizeRel)$",
                     args.gti_build_cache.read_text(), re.M)
    output.mkdir(parents=True)
    source = output / f"visibility_{args.case}.gti"
    shutil.copyfile(fixtures / source.name, source)
    checked_line = next(index for index, line in enumerate(source.read_text().splitlines(), 1)
                        if line.startswith("int checked("))
    manifest = {"status": "running", "commands": [], "observations": [],
                "source_sha256": hashlib.sha256(source.read_bytes()).hexdigest(),
                "gti_sha256": hashlib.sha256(args.gti.read_bytes()).hexdigest(),
                "case": args.case, "limits": ["focused correctness, not cost acceptance",
                                                "both cases require native-C scalar declaration admission",
                                                "callable case requires explicitly composed O09 dependency"]}

    def run(argv, *, fail=None):
        env = dict(os.environ, NO_COLOR="1", GTI_COLOR="never")
        if fail is not None:
            env["GTI_VISIBILITY_FAIL"] = str(int(fail))
        result = subprocess.run([str(x) for x in argv], cwd=root, env=env,
                                capture_output=True, text=True, timeout=180)
        log = f"command-{len(manifest['commands']):03}.log"
        (output / log).write_text(result.stdout + result.stderr)
        manifest["commands"].append({"argv": [str(x) for x in argv],
                                     "exit": result.returncode, "log": log,
                                     "failure_selected": fail})
        if fail is None:
            assert result.returncode == 0, str(output / log)
        return result

    try:
        manifest["head"] = run(["git", "rev-parse", "HEAD"]).stdout.strip()
        manifest["dirty"] = run(["git", "status", "--short"]).stdout
        manifest["gti_version"] = run([args.gti, "--version"]).stdout.strip()
        run([args.clang, "-std=c17", "-O2", "-c", fixtures / "visibility_provider.c",
             "-o", output / "provider.o"])
        endpoints = [("cpp", 3), *(("llvm", level) for level in range(4))]
        for backend, level in endpoints:
            binary = output / f"{backend}-O{level}"
            command = [args.gti, source, "--backend", backend, f"-O{level}",
                       "--progress", "never", "-o", binary]
            if backend == "cpp":
                command += ["--std", "c++23"]
            run([*command, "--", output / "provider.o"])
            for fail in (False, True):
                result = run([binary], fail=fail)
                expected = "drop:2\ndrop:1\n"
                if fail:
                    expected += (f"{source.name}:{checked_line}: runtime error[GTI-R0001]: "
                                 "integer_overflow in addition\n")
                assert result.returncode == (70 if fail else 0)
                assert not result.stdout and result.stderr == expected, result.stderr
                manifest["observations"].append({"backend": backend, "level": level,
                                                 "failure": fail, "exit": result.returncode,
                                                 "stderr": result.stderr})
        manifest["status"] = "passed-selected-production-correctness"
        print(f"PASS: {args.case}, C++23/O3 and LLVM/O0-O3 exact success/failure cleanup")
    except BaseException as error:
        manifest["status"], manifest["error"] = "failed", str(error)
        raise
    finally:
        (output / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")


if __name__ == "__main__":
    main()
