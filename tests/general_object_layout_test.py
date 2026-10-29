#!/usr/bin/env python3
"""General constants agree with actual native storage at every optimization level."""

import os
from pathlib import Path
import subprocess
import sys
import tempfile


def main():
    compiler = Path(sys.argv[1]).resolve()
    root = Path(__file__).resolve().parent.parent
    env = {**os.environ, "GTI_STDLIB_PATH": str(root / "stdlib")}
    sources = [root / "examples/181-general-object-layout.gti",
               root / "tests/fixtures/general_object_layout.gti"]
    with tempfile.TemporaryDirectory(prefix="gti-general-layout-") as directory:
        for source in sources:
            for level in ("O0", "O1", "O2", "O3"):
                binary = Path(directory) / f"{source.stem}-{level}"
                command = [str(compiler), str(source), "--backend", "llvm",
                           f"-{level}", "-o", str(binary)]
                built = subprocess.run(command, capture_output=True, text=True,
                                       env=env, timeout=120)
                assert built.returncode == 0, (command, built.stdout, built.stderr)
                result = subprocess.run([str(binary)], capture_output=True,
                                        text=True, timeout=30)
                assert (result.returncode, result.stdout, result.stderr) == (0, "", ""), result
                print(f"general object layout: {source.name} {level} passed", flush=True)
            binary.write_bytes(b"preserve existing artifact")
            rejected = subprocess.run(
                [str(compiler), str(source), "--backend", "cpp", "-o", str(binary)],
                capture_output=True, text=True, env=env, timeout=120)
            assert (rejected.returncode == 65 and "GTI-B0003" in rejected.stderr
                    and "general sizeof/alignof" in rejected.stderr
                    and binary.read_bytes() == b"preserve existing artifact"), rejected


if __name__ == "__main__":
    main()
