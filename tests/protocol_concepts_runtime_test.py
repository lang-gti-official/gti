#!/usr/bin/env python3
"""A protocol concept's operation executes as a call of the selected block.

The fixture traces a concrete and a generic conformance block in every mode:
calls through a requirement in generic code, block functions calling each
other, associated types used for locals and results, and operations that take
ownership of an owning operand, which is released exactly once when the
operation returns or fails. Example 156 runs as documented. A two-package workspace
checks package coherence: a package may supply a block for a concept or a
type it owns, and not for a dependency's concept and type together.
"""

import pathlib
import subprocess
import sys
import tempfile

from shared_failure_cleanup_runtime_test import SITES, expected_report
from unique_class_allocation_runtime_test import run

# (phase, exit code, stdout integers, report)
CASES = [
    (0, 0, [1, 101, 101, -1, 1, 2, 10, -2, 20, -101], None),
    (1, 70, [3, -3], ("ordinary", "scale")),
]

SITES["protocol_concepts.gti"] = {
    "scale": "int scale() { return 10 / this.divisor; }",
}

LIBRARY = """concept shaped<T> {
  typename size_type;
  size_type size(T& value);
};
class Square {
public:
  int side;
  Square(int length) : side(length) {}
};
"""

OWNED = """#include <lib/shapes>
class Circle {
public:
  int radius;
  Circle(int length) : radius(length) {}
};
// This package owns the type.
for concept shaped<Circle> {
  using size_type = int;
  int size(Circle& value) { return value.radius; }
};
concept local<T> {
  typename item;
  item get(T& value);
};
// This package owns the concept.
for concept local<Square> {
  using item = int;
  int get(Square& value) { return value.side; }
};
int main() {
  Circle circle{3};
  Square square{4};
  return shaped<Circle>::size(circle) + local<Square>::get(square) - 7;
}
"""

FOREIGN = """#include <lib/shapes>
for concept shaped<Square> {
  using size_type = int;
  int size(Square& value) { return value.side; }
};
int main() { return 0; }
"""


def coherence(compiler, directory):
    root = pathlib.Path(directory) / "coherence"
    library = root / "lib"
    (library / "src").mkdir(parents=True)
    (library / "gti.toml").write_text(
        'manifest-version = 1\n[package]\nname = "lib"\nversion = "0.1.0"\n')
    (library / "src/shapes.gti").write_text(LIBRARY)
    app = root / "app"
    app.mkdir()
    (app / "gti.toml").write_text(
        'manifest-version = 1\n[package]\nname = "app"\nversion = "0.1.0"\n'
        '[dependencies]\nlib = { path = "../lib" }\n'
        '[targets.app]\nkind = "executable"\nroot = "main.gti"\n')
    (app / "main.gti").write_text(OWNED)
    built = subprocess.run([str(compiler), "build"], cwd=app,
                           capture_output=True, text=True, timeout=600)
    assert built.returncode == 0, (built.stdout, built.stderr)
    executable = next(path for path in (app / "build").rglob("app*")
                      if path.is_file() and path.name in ("app", "app.exe"))
    assert run([str(executable)]).returncode == 0
    (app / "main.gti").write_text(FOREIGN)
    rejected = subprocess.run([str(compiler), "check"], cwd=app,
                              capture_output=True, text=True, timeout=600)
    assert rejected.returncode != 0 and "GTI-S2123" in rejected.stderr and \
        "owns neither the concept nor the type" in rejected.stderr, rejected.stderr
    print("protocol concepts: package coherence passed", flush=True)


def main():
    compiler = pathlib.Path(sys.argv[1]).resolve()
    tests = pathlib.Path(__file__).parent
    source = tests / "fixtures" / "protocol_concepts.gti"
    example = tests.parent / "examples" / "156-protocol-concepts.gti"
    modes = [(f"{standard}-{opt}", ["--std", standard, f"-{opt}"], [])
             for standard in ("c++20", "c++23") for opt in ("O0", "O1", "O2", "O3")]
    modes.append(("sanitized", ["--std", "c++23", "-O1"],
                  ["--", "-fsanitize=address,undefined", "-fno-omit-frame-pointer"]))
    with tempfile.TemporaryDirectory(prefix="gti-protocol-concepts-") as directory:
        for name, flags, native in modes:
            binary = pathlib.Path(directory) / f"protocol-concepts-{name}"
            command = [str(compiler), str(source), *flags, "-o", str(binary), *native]
            built = run(command)
            assert built.returncode == 0, (command, built.stdout, built.stderr)
            for phase, code, trace, report in CASES:
                result = run([str(binary), *(["phase"] * phase)])
                actual = (result.returncode, [int(item) for item in result.stdout.split()],
                          result.stderr)
                wanted = (code, trace, expected_report(source, report))
                assert actual == wanted, (name, phase, actual, wanted)
            demo = pathlib.Path(directory) / f"example-{name}"
            built = run([str(compiler), str(example), *flags, "-o", str(demo), *native])
            assert built.returncode == 0, (name, built.stdout, built.stderr)
            shown = run([str(demo)])
            assert (shown.returncode, shown.stdout) == (0, "a rope\na crate\n42\n"), \
                (name, shown.returncode, shown.stdout, shown.stderr)
            print(f"protocol concepts: {name} passed", flush=True)
        coherence(compiler, directory)


if __name__ == "__main__":
    main()
