#!/usr/bin/env python3
"""Runtime evidence for `constexpr for` and <std/meta> across optimization levels and C++ standards."""

import pathlib
import subprocess
import sys
import tempfile
from backend_modes import cpp_text_modes, native_modes


# Field names a program never uses at run time must not survive into its
# representation: reflection leaves no tables behind.
RESIDUE = {
    "reflection_basic": ['"health"', '"speed"', '"alive"'],
    "reflection_binary_writer": ['"alive"', '"position"', '"count"'],
    "reflection_generic_encoder": ['"health"', '"position"'],
}


def check_residue(compiler: pathlib.Path, source: pathlib.Path, root: pathlib.Path) -> bool:
    """No unused field name reaches verified MIR as a string literal, and none
    survives as a standalone string in the native executable; under the C++
    profile, none appears in the emitted C++ text either."""
    names = [literal.strip('"') for literal in RESIDUE[source.stem]]
    mir_output = root / f"{source.stem}.mir"
    result = subprocess.run(
        [str(compiler), str(source), "--emit-mir", "-o", str(mir_output)],
        capture_output=True, text=True)
    if result.returncode != 0:
        print(f"failed: emit MIR {source}\n{result.stdout}{result.stderr}", file=sys.stderr)
        return False
    mir = mir_output.read_text(encoding="utf-8", errors="replace")
    for name in names:
        encoded = name.encode("utf-8")
        if f"literal=string:{len(encoded)}:{encoded.hex()}" in mir:
            print(f"reflection residue: \"{name}\" literal in verified MIR of {source}",
                  file=sys.stderr)
            return False
    for mode, flags, _native in native_modes():
        executable = root / f"{source.stem}-residue-{mode}"
        built = subprocess.run([str(compiler), str(source), *flags, "-o", str(executable)],
                               capture_output=True, text=True)
        if built.returncode != 0:
            print(f"failed: build {source} {mode}\n{built.stdout}{built.stderr}", file=sys.stderr)
            return False
        image = executable.read_bytes()
        for name in names:
            if b"\0" + name.encode("utf-8") + b"\0" in image:
                print(f"reflection residue: \"{name}\" string in the {mode} executable of {source}",
                      file=sys.stderr)
                return False
    for _label, cpp_flags, _native in cpp_text_modes():
        output = root / f"{source.stem}.cpp"
        result = subprocess.run(
            [str(compiler), str(source), *cpp_flags, "--emit-cpp", "-o", str(output)],
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
            for mode, flags, native in native_modes():
                executable = root / f"{source.stem}-{mode}"
                for command in (
                    [str(compiler), str(source), *flags, "-o", str(executable)],
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
