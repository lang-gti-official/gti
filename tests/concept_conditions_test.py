#!/usr/bin/env python3
"""Named concept values share constraints, staging, and constant MIR lowering."""
import os
import pathlib
import subprocess
import sys
import tempfile


def main():
    compiler = pathlib.Path(sys.argv[1]).resolve()
    root = pathlib.Path(__file__).resolve().parent.parent
    env = dict(os.environ, GTI_STDLIB_PATH=str(root / "stdlib"))

    def run(args):
        result = subprocess.run([str(compiler), *map(str, args)], env=env,
                                text=True, capture_output=True, timeout=120)
        return result

    prefix = "concept sized<T> = requires(T& v) { { v.size() } -> uint64_t; };\n"
    negatives = {
        "unknown": ("static_assert(absent<int>);", "GTI-S2049", "Unknown concept 'absent'"),
        "not_concept": ("struct Box { Box() = default; }; static_assert(Box<int>);",
                        "GTI-S2049", "Unknown concept 'Box'"),
        "wrong_arity": ("static_assert(std::integral<int, float>);", "GTI-S2049", "requires 1 type argument"),
        "zero_arity": ("static_assert(std::integral<>);", "GTI-S2049", "requires 1 type argument"),
        "false_assertion": ("static_assert(sized<int>);", "GTI-S2079", "Static assertion failed"),
        "else_members": ("int f<T>(T& v) { if constexpr(sized<T>) { return 0; } "
                         "else { return int(v.size()); } } int main() { int x = 1; return f(x); }",
                         "GTI-S2000", "Member access requires"),
        "disjunction_members": ("int f<T>(T& v) { if constexpr(sized<T> || std::integral<T>) "
                                "{ return int(v.size()); } else { return 0; } } "
                                "int main() { int x = 1; return f(x); }",
                                "GTI-S2000", "Member access requires"),
        "negation_members": ("int f<T>(T& v) { if constexpr(!sized<T>) { return int(v.size()); } "
                             "else { return 0; } } int main() { int x = 1; return f(x); }",
                             "GTI-S2000", "Member access requires"),
        "constraint_overloads": ("T pick<std::integral T>(T a) { return a; } "
                                 "T pick<std::floating_point T>(T a) { return a; }",
                                 "GTI-S2011", "Duplicate overload signature"),
        "nested_requires": ("concept nested<T> = requires(T& v) { requires sized<T>; };",
                            "GTI-P0001", "required expression"),
        "boolean_helper_facts": ("constexpr bool query<T>() { return requires(T& v) "
                                 "{ { v.size() } -> uint64_t; }; } "
                                 "concept indirect<T> = query<T>(); "
                                 "uint64_t read<indirect T>(T& v) { return v.size(); }",
                                 "GTI-S2000", "Member access requires"),
        "comparison": ("int main() { int a=1; int b=2; int c=3; "
                       "bool comparison = a < b > +c; return 0; }",
                       "GTI-S2004", "Operator '>'"),
    }
    with tempfile.TemporaryDirectory(prefix="gti-concept-conditions-") as temporary:
        directory = pathlib.Path(temporary)
        for label, (source, code, message) in negatives.items():
            path = directory / f"{label}.gti"
            path.write_text(prefix + source)
            result = run([path, "--emit-mir", "-o", directory / f"{label}.mir"])
            assert result.returncode != 0 and code in result.stderr and message in result.stderr, (
                label, result.returncode, result.stdout, result.stderr)
            assert f"{label}.gti:" in result.stderr, result.stderr
            assert "internal compiler error" not in result.stderr.lower(), result.stderr

        for source in (root / "tests/fixtures/concept_conditions.gti",
                       root / "examples/188-concept-conditions.gti"):
            mir = directory / f"{source.stem}.mir"
            result = run([source, "--emit-mir", "-o", mir])
            assert result.returncode == 0, result.stderr
            assert " valid=1\n" in mir.read_text()[:80]
            # Query results are Boolean literals; no runtime concept operation
            # is introduced. Native success checks every chosen branch.
            for backend, level in (("llvm", "O0"), ("llvm", "O2"), ("cpp", "O2")):
                binary = directory / f"{source.stem}-{backend}-{level}"
                result = run([source, "--backend", backend, f"-{level}", "-o", binary])
                assert result.returncode == 0, (backend, result.stderr)
                executed = subprocess.run([str(binary)], capture_output=True, text=True, timeout=20)
                assert (executed.returncode, executed.stdout, executed.stderr) == (0, "", ""), executed
    print("concept conditions: semantic boundaries, verified MIR, LLVM O0/O2 and C++ O2 passed")


if __name__ == "__main__":
    main()
