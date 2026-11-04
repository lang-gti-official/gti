#!/usr/bin/env python3
"""Recursive protocol requirements: native dispatch, rejection, and bounded cycles."""
import pathlib
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[1]
COMPILER = pathlib.Path(sys.argv[1]).resolve()
BASE = (ROOT / "tests/fixtures/protocol_requirements.gti").read_text()
PREFIX = BASE[:BASE.index("int main()")]


def run(args, *, success=True, fragment=None):
    result = subprocess.run(args, text=True, capture_output=True, timeout=90)
    output = result.stdout + result.stderr
    if (result.returncode == 0) != success or (fragment and fragment not in output):
        raise AssertionError(f"command: {args}\n{output}")
    return output


with tempfile.TemporaryDirectory(prefix="gti-protocol-requirements-") as temporary:
    directory = pathlib.Path(temporary)
    source = directory / "main.gti"
    source.write_text(BASE)
    for optimization in ("-O0", "-O3"):
        executable = directory / optimization[1:]
        run([str(COMPILER), str(source), "--backend", "llvm", optimization,
             "-o", str(executable)])
        run([str(executable)])
    failures = {
        "nested": (PREFIX + "struct Opaque { Opaque() = default; };\nint main() { "
                   "Box<Box<Opaque>> value{Box<Opaque>(Opaque())}; "
                   "return read_score(value); }", "none matches 'Opaque'"),
        "direct": (PREFIX + "struct Opaque { Opaque() = default; };\nint main() { "
                   "Box<Opaque> value{Opaque()}; "
                   "return score<Box<Opaque>>::read(value); }", "none matches 'Opaque'"),
        "cycle": ("concept C<T> { int get(T& v); }; struct X { X() = default; }; "
                  "for concept C<X> requires C<X> { int get(X& v) { return 0; } }; "
                  "int main() { X x{}; return C<X>::get(x); }", "cyclic"),
        "expansion": ("concept C<T> { int get(T& v); }; struct X<T> { X() = default; }; "
                      "for <T> concept C<X<T>> requires C<X<X<T>>> { "
                      "int get(X<T>& v) { return 0; } }; "
                      "int main() { X<int> x{}; return C<X<int>>::get(x); }", "nesting limit"),
        "doubling": ("concept C<T> { int get(T& v); }; struct X<T> { X() = default; }; "
                     "struct Pair<A, B> {}; "
                     "for <T> concept C<X<T>> requires C<X<Pair<T, T>>> { "
                     "int get(X<T>& v) { return 0; } }; "
                     "int main() { X<int> x{}; return C<X<int>>::get(x); }", "type-expansion budget"),
        "overlap": (PREFIX + "for <T> concept score<Box<T>> requires std::integral<T> { "
                    "int read(Box<T>& v) { return 0; } }; int main() { return 0; }", "overlap"),
    }
    for name, (contents, expected) in failures.items():
        source.write_text(contents)
        output = run([str(COMPILER), str(source), "--emit-mir", "-o",
                      str(directory / (name + ".mir"))], success=False, fragment=expected)
        if "error[GTI-S" not in output:
            raise AssertionError(output)
print("protocol requirements: LLVM O0/O3 and 6 bounded rejection cases passed")
