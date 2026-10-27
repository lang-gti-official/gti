#!/usr/bin/env python3
"""Run generated-GTI memory checks with retained, uniquely named evidence."""

import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import uuid


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--probe", required=True, type=Path)
    parser.add_argument("--build-cache", required=True, type=Path)
    parser.add_argument("--cxx", required=True, type=Path)
    parser.add_argument("--cc", required=True, type=Path)
    parser.add_argument("--evidence-root", required=True, type=Path)
    args = parser.parse_args()
    cache = args.build_cache.resolve()
    probe = args.probe.resolve()
    assert probe.parent == cache.parent
    assert re.search(r"^CMAKE_BUILD_TYPE:STRING=(Release|RelWithDebInfo|MinSizeRel)$",
                     cache.read_text(), re.M), "probe must use an optimized host compiler build"
    output = args.evidence_root.resolve() / ("run-" + uuid.uuid4().hex)
    output.parent.mkdir(parents=True, exist_ok=True)
    command = [str(probe), "--generated-memory-probe", str(args.cxx), str(args.cc), str(output)]

    def digest(path):
        with path.open("rb") as stream:
            return hashlib.file_digest(stream, "sha256").hexdigest()

    manifest = {"status": "running", "command": command,
                "probe_sha256": digest(probe), "build_cache_sha256": digest(cache),
                "script_sha256": digest(Path(__file__)), "toolchains": []}
    try:
        for compiler in (args.cxx, args.cc):
            version = subprocess.run([str(compiler), "--version"], capture_output=True,
                                     text=True, timeout=30, check=True)
            manifest["toolchains"].append({"path": str(compiler),
                                           "resolved": str(compiler.resolve()),
                                           "sha256": digest(compiler),
                                           "version": version.stdout + version.stderr})
        result = subprocess.run(command, capture_output=True, text=True, timeout=600)
        output.mkdir(parents=True, exist_ok=True)
        (output / "probe.log").write_text(result.stdout + result.stderr)
        manifest["exit"] = result.returncode
        manifest["status"] = "passed" if result.returncode == 0 else "failed"
        print(f"Generated-memory gate {manifest['status']}; evidence: {output}")
        return result.returncode
    except BaseException as error:
        manifest["status"], manifest["error"] = "failed", str(error)
        raise
    finally:
        output.mkdir(parents=True, exist_ok=True)
        (output / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")


if __name__ == "__main__":
    raise SystemExit(main())
