#!/usr/bin/env python3
"""Require the executable host backend in the configured package closure."""

from pathlib import Path
import re
import sys


def main():
    if len(sys.argv) != 3:
        raise SystemExit("usage: llvm_cache_component_mapping_test.py build-root processor")
    build = Path(sys.argv[1]).resolve()
    native = {"arm64": "AArch64", "aarch64": "AArch64",
              "x86_64": "X86", "amd64": "X86"}[sys.argv[2].lower()]
    components = (build / "gti-llvm-backend-components.txt").read_text().splitlines()
    assert len(components) == len(set(components)), components
    assert all(re.fullmatch(r"LLVM[A-Za-z0-9_]+", value)
               for value in components), components
    required = {"LLVMCore", "LLVMPasses",
                *(f"LLVM{native}{suffix}" for suffix in ("CodeGen", "Desc", "Info"))}
    missing = required - set(components)
    assert not missing, f"Configured native backend closure is missing {sorted(missing)}"
    print(f"Canonical {native} native roots present in {len(components)}-component closure")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
