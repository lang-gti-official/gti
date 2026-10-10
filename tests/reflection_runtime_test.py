#!/usr/bin/env python3
"""Runtime evidence for `constexpr for` and <std/meta> across optimization levels and C++ standards."""

import pathlib
import subprocess
import sys
import tempfile


# Field names a program never uses at run time must not survive into its
# representation: reflection leaves no tables behind.
RESIDUE = {
    "reflection_basic": ['"health"', '"speed"', '"alive"'],
    "reflection_binary_writer": ['"alive"', '"position"', '"count"'],
    "reflection_generic_encoder": ['"health"', '"position"'],
}


def check_residue(compiler: pathlib.Path, source: pathlib.Path, root: pathlib.Path) -> bool:
    output = root / f"{source.stem}.cpp"
    result = subprocess.run(
        [str(compiler), str(source), "--emit-cpp", "-o", str(output)],
        capture_output=True, text=True)
    if result.returncode != 0:
        print(f"failed: emit {source}\n{result.stdout}{result.stderr}", file=sys.stderr)
        return False
    text = output.read_text(encoding="utf-8", errors="replace")
    for literal in RESIDUE[source.stem]:
        if literal in text:
            print(f"reflection residue: {literal} in representation of {source}",
                  file=sys.stderr)
            return False
    return True


def main() -> int:
    compiler = pathlib.Path(sys.argv[1]).resolve()
    sources = [pathlib.Path(argument).resolve() for argument in sys.argv[2:]]
    with tempfile.TemporaryDirectory(prefix="gti-reflection-") as temporary:
        root = pathlib.Path(temporary)
        for source in sources:
            if source.stem in RESIDUE and not check_residue(compiler, source, root):
                return 1
            for optimization in ("O0", "O1", "O3"):
                for standard in ("c++20", "c++23"):
                    executable = root / f"{source.stem}-{optimization}-{standard}"
                    for command in (
                        [str(compiler), str(source), f"-{optimization}", "--std",
                         standard, "-o", str(executable)],
                        [str(executable)],
                    ):
                        result = subprocess.run(command, capture_output=True, text=True)
                        if result.returncode != 0 or (
                            len(command) == 1 and (result.stdout or result.stderr)
                        ):
                            print(f"failed: {command}\n{result.stdout}{result.stderr}",
                                  file=sys.stderr)
                            return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
