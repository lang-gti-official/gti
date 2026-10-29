#!/usr/bin/env python3
"""Owned collection braces for sequences, FIFO and unique-key containers."""
import argparse
import os
import pathlib
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("compiler", type=pathlib.Path)
    parser.add_argument("--fixture", action="append", choices=(
        "complete_containers_collection", "complete_containers_ownership",
        "complete_containers_collection_failure", "mixed_generic_field_initializers"))
    arguments = parser.parse_args()
    compiler = arguments.compiler.resolve()
    repository = pathlib.Path(__file__).resolve().parent.parent
    fixtures = repository / "tests/fixtures"
    environment = dict(os.environ, GTI_STDLIB_PATH=str(repository / "stdlib"),
                       ASAN_OPTIONS="halt_on_error=1", UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1")

    def run(command):
        return subprocess.run(list(map(str, command)), env=environment, text=True,
                              capture_output=True, timeout=180)

    with tempfile.TemporaryDirectory(prefix="gti-container-collection-") as directory:
        root = pathlib.Path(directory)
        negatives = {}
        for kind in ("list", "deque", "queue", "set", "unordered_set"):
            # Owning string is a valid comparable/hashable move-only element.
            negatives[f"copied-{kind}"] = (f"""
import std.{kind};
import std.string;
int main() {{
  std::string named{{"owned"}};
  std::{kind}<std::string> values = {{named}};
  return int(values.size());
}}
""", "GTI-S2092")
        negatives["nondefault-order"] = ("""
import std.set;
class Policy {
  int value;
public:
  Policy(int value) : value(value) {}
  bool operator()(int& left, int& right) { return left < right; }
};
int main() { std::set<int, Policy> values = {1, 2}; return int(values.size()); }
""", "GTI-S2029")
        negatives["nondefault-hash"] = ("""
import std.unordered_set;
class Policy {
  int value;
public:
  Policy(int value) : value(value) {}
  std::size_t operator()(int& key) { return std::size_t(key); }
};
int main() { std::unordered_set<int, Policy> values = {1, 2}; return int(values.size()); }
""", "GTI-S2029")
        for name, (source, code) in negatives.items():
            path = root / f"{name}.gti"
            path.write_text(source)
            checked = run([compiler, path, "--emit-mir", "-o", root / "negative.mir"])
            assert checked.returncode == 65 and f"error[{code}]" in checked.stderr, (name, checked)
            assert "GTI-B0001" not in checked.stderr, (name, checked)
        print("container collection: 7 ownership/policy negatives passed", flush=True)

        modes = [(f"{standard}-{level}", ["--std", standard, f"-{level}"], [])
                 for standard in ("c++20", "c++23") for level in ("O0", "O1", "O3")]
        modes += [("no-elide", ["--std", "c++23", "-O0"], ["-fno-elide-constructors"]),
                  ("sanitized", ["--std", "c++23", "-O1"],
                   ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"])]
        for label, flags, native in modes:
            for name in arguments.fixture or ("complete_containers_collection", "complete_containers_ownership",
                                               "complete_containers_collection_failure", "mixed_generic_field_initializers"):
                binary = root / name
                command = [compiler, fixtures / f"{name}.gti", *flags, "-o", binary]
                if native:
                    command += ["--", *native]
                built = run(command)
                assert built.returncode == 0, (label, name, built)
                if name.endswith("_failure"):
                    for count in range(7):
                        result = run([binary, *(["failure"] * count)])
                        assert result.returncode == 70 and result.stdout == "1\n2\n-2\n-1\n99\n", (label, count, result)
                        assert "runtime error[GTI-R0002]: division_by_zero" in result.stderr, result
                else:
                    result = run([binary])
                    assert (result.returncode, result.stdout, result.stderr) == (0, "", ""), (label, name, result)
            print(f"container collection: {label} passed", flush=True)


if __name__ == "__main__":
    main()
