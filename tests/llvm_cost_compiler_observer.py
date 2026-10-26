#!/usr/bin/env python3
"""Retain actual benchmark compiler inputs/outputs while forwarding unchanged."""

import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys


def digest(path):
    value = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            value.update(block)
    return value.hexdigest()


def observe(compiler, capture_root, arguments):
    # Preserve argv[0]: clang++ and clang may resolve to the same executable
    # while selecting different driver/link modes.
    compiler, capture_root = Path(compiler).absolute(), Path(capture_root).resolve()
    # Preserve compiler/dependency queries without pretending they are builds.
    if "-o" not in arguments or any(flag in arguments for flag in
            ("-E", "-M", "-MM", "-###", "-fsyntax-only")):
        return subprocess.call([str(compiler), *arguments])
    output = Path(arguments[arguments.index("-o") + 1]).resolve()
    capture = capture_root / output.name
    capture.mkdir(parents=True, exist_ok=False)
    inputs = []
    for argument in arguments:
        path = Path(argument)
        if path.suffix not in (".cpp", ".cc", ".c", ".o", ".a") or not path.is_file():
            continue
        path = path.resolve()
        row = {"original": str(path), "bytes": path.stat().st_size, "sha256": digest(path)}
        if path.suffix != ".a":
            target = capture / f"input-{len(inputs):03}-{path.name}"
            shutil.copyfile(path, target)
            row["retained"] = target.name
        inputs.append(row)
    record = {"schema": 1, "cwd": str(Path.cwd()),
              "compiler": {"path": str(compiler), "implementation_path": str(compiler.resolve()),
                           "sha256": digest(compiler)},
              "argv": [str(compiler), *arguments], "inputs": inputs,
              "environment": {name: os.environ[name] for name in
                  ("CPATH", "CPLUS_INCLUDE_PATH", "C_INCLUDE_PATH", "LIBRARY_PATH",
                   "SDKROOT", "MACOSX_DEPLOYMENT_TARGET", "CCC_OVERRIDE_OPTIONS",
                   "CLANG_CONFIG_FILE_SYSTEM_DIR", "CLANG_CONFIG_FILE_USER_DIR") if name in os.environ},
              "outputs": []}
    result = subprocess.run(record["argv"], capture_output=True)
    record["exit"] = result.returncode
    (capture / "stdout.log").write_bytes(result.stdout)
    (capture / "stderr.log").write_bytes(result.stderr)
    changed = [row["original"] for row in inputs
               if digest(Path(row["original"])) != row["sha256"]]
    record["changed_inputs"] = changed
    # -save-temps=obj outputs live in the actual publication staging directory.
    # Retain them before GTI's normal atomic-publication cleanup removes it.
    for path in sorted(output.parent.iterdir()):
        if not path.is_file() or (path != output and path.suffix not in (".bc", ".ll", ".ii", ".s", ".o")):
            continue
        target = capture / f"output-{len(record['outputs']):03}-{path.name}"
        shutil.copyfile(path, target)
        record["outputs"].append({"original": str(path), "retained": target.name,
                                  "bytes": path.stat().st_size, "sha256": digest(path)})
    (capture / "record.json").write_text(json.dumps(record, indent=2) + "\n")
    sys.stdout.buffer.write(result.stdout)
    sys.stderr.buffer.write(result.stderr)
    if changed:
        print("benchmark compiler input changed during compilation", file=sys.stderr)
        return 125
    return result.returncode
