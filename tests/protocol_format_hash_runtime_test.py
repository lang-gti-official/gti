#!/usr/bin/env python3
"""Source protocol clients, legacy adapters, nested pairs, and constraint failures."""
import pathlib
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[1]
COMPILER = pathlib.Path(sys.argv[1]).resolve()


def invoke(args, *, success=True, expected=None):
    result = subprocess.run(args, text=True, capture_output=True, timeout=180)
    output = result.stdout + result.stderr
    if (result.returncode == 0) != success or (expected and expected not in output):
        raise AssertionError(f"command: {args}\n{output}")
    return result.stdout


with tempfile.TemporaryDirectory(prefix="gti-protocol-format-hash-") as temporary:
    directory = pathlib.Path(temporary)
    for fixture, output in (("tests/fixtures/protocol_format_hash.gti", "(8, 9)\n"),
                            ("examples/189-protocol-format-hash.gti", "(1, true)\nbox(7)\n")):
        for optimization in ("-O0", "-O3"):
            executable = directory / (pathlib.Path(fixture).stem + optimization)
            invoke([str(COMPILER), str(ROOT / fixture), "--backend", "llvm", optimization,
                    "-o", str(executable)])
            actual = invoke([str(executable)])
            if actual != output:
                raise AssertionError(f"unexpected output: {actual!r}")
    failures = {
        "format": ("import std.format; import std.utility; struct Opaque { Opaque() = default; }; "
                   "int main() { auto p = std::make_pair(Opaque(), 1); "
                   "std::println(\"{}\", p); return 0; }", "std::formattable"),
        "hash": ("import std.functional; import std.utility; struct Opaque { Opaque() = default; }; "
                 "int main() { auto p = std::make_pair(Opaque(), 1); "
                 "std::default_hash<std::pair<Opaque, int>> h{}; h(p); return 0; }", "std::hashable"),
        "map_key": ("import std.unordered_map; import std.utility; struct Opaque { Opaque() = default; "
                    "bool operator==(Opaque& other) { return true; } "
                    "bool operator!=(Opaque& other) { return false; } }; "
                    "int main() { std::unordered_map<std::pair<Opaque, int>, int> values{}; "
                    "return 0; }", "std::read_callable"),
        "specialization": ("import std.format; import std.utility; class std::formatter<std::pair<T, U>> {}; "
                           "int main() { return 0; }", "GTI-S2078"),
    }
    for name, (source, diagnostic) in failures.items():
        path = directory / (name + ".gti")
        path.write_text(source)
        invoke([str(COMPILER), str(path), "--emit-mir", "-o", str(directory / (name + ".mir"))],
               success=False, expected=diagnostic)
print("protocol formatting/hashing: fixtures and example LLVM O0/O3, 4 rejections passed")
