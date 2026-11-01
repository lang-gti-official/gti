#!/usr/bin/env python3
"""A returned owner keeps a by-value carrier parameter's referent alive."""

import os
from pathlib import Path
import subprocess
import sys
import tempfile


def run(*args):
    return subprocess.run([str(arg) for arg in args], capture_output=True,
                          text=True, timeout=120)


def main():
    compiler = Path(sys.argv[1]).resolve()
    repository = Path(sys.argv[2]).resolve()
    os.environ["GTI_STDLIB_PATH"] = str(repository / "stdlib")
    fixtures = repository / "tests/fixtures"
    with tempfile.TemporaryDirectory(prefix="gti-carrier-return-") as tmp:
        root = Path(tmp)
        general = (fixtures / "borrowed_carrier_return.gti").read_text()
        vector = (fixtures / "resource_vector_return.gti").read_text()
        direct = root / "direct-resource-binding.gti"
        direct.write_text(vector.replace("make(binding)",
                                         "make(std::bind_resource(heap))"))
        invalid = root / "borrowed-resource-mutation.gti"
        invalid.write_text(general.replace("  return owner.read() - 2;",
                                           "  resource.reset();\n  return owner.read() - 2;"))
        for backend in ("llvm", "cpp"):
            for level in ("O0", "O3"):
                for source in (fixtures / "borrowed_carrier_return.gti",
                               fixtures / "resource_vector_return.gti", direct):
                    executable = root / f"{source.stem}-{backend}-{level}"
                    built = run(compiler, source, "--backend", backend,
                                f"-{level}", "--progress", "never", "-o", executable)
                    assert built.returncode == 0, (source, backend, level, built.stderr)
                    result = run(executable)
                    assert (result.returncode, result.stdout, result.stderr) == (0, "", ""), (
                        source, backend, level, result)
            rejected = run(compiler, invalid, "--backend", backend,
                           "--progress", "never", "-o", root / "rejected")
            assert rejected.returncode == 65 and "GTI-S2017" in rejected.stderr, (
                backend, rejected.stderr)


if __name__ == "__main__":
    main()
