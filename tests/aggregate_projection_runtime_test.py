#!/usr/bin/env python3
"""Fixed-array field projections and class decomposition preserve safety."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile


def run(command, *, expected=0):
    result = subprocess.run(command, capture_output=True, text=True, timeout=90)
    if result.returncode != expected:
        raise AssertionError(f"{command}: {result.returncode}\n{result.stdout}\n{result.stderr}")
    return result


def main():
    compiler, repository = Path(sys.argv[1]).resolve(), Path(sys.argv[2]).resolve()
    os.environ["GTI_STDLIB_PATH"] = str(repository / "stdlib")
    sources = [repository / f"tests/fixtures/{name}.gti" for name in
               ("array_field_projections", "class_array_decomposition",
                "owning_class_array_decomposition", "constructor_struct_field_address",
                "class_array_arguments", "array_element_references",
                "checked_class_copy_arguments", "guarded_class_array_decomposition")] + [
        repository / f"tests/coverage/reproducers/{name}.gti"
        for name in ("a39-ctor-address-of-struct-field", "a40-raw-pointer-fixed-array-field-index",
                     "a40-c-abi-record-pointer-array-field",
                     "a51-binding-over-class-array", "a51-binding-over-class-array-invalid-cpp")
    ] + [repository / f"examples/{name}.gti" for name in (
        "032-structured-bindings", "012-fixed-arrays", "111-final-address-storage")]
    with tempfile.TemporaryDirectory(prefix="gti-aggregate-projection-") as directory:
        scratch = Path(directory)
        native = scratch / "native.o"
        run(["cc", "-std=c17", "-c", str(repository / "tests/coverage/native/projection_records.c"), "-o", str(native)])
        for standard in ("c++20", "c++23"):
            for optimization in ("O0", "O1", "O3"):
                for source in sources:
                    executable = scratch / f"{source.stem}-{standard}-{optimization}"
                    run([str(compiler), str(source), "--std", standard,
                         f"-{optimization}", "--progress", "never", "-o", str(executable),
                         "--", str(native), "-fno-elide-constructors"])
                    executed = run([str(executable)])
                    expected_output = ("structured bindings worked\n" if source.name.startswith("032-") else
                                       "fixed arrays keep their bounds\n" if source.name.startswith("012-") else "")
                    assert executed.stdout == expected_output and executed.stderr == "", executed
        for source in sources[:8]:
            executable = scratch / f"{source.stem}-sanitized"
            run([str(compiler), str(source), "-O1", "--progress", "never", "-o", str(executable),
                 "--", str(native), "-fsanitize=address,undefined", "-fno-elide-constructors"])
            run([str(executable)])
        prefix = ("class Guard{public:Guard(){}~Guard(){std::println(\"guard dropped\");}};"
                  "struct Frame{mut int slots[2]={1,2};};int outside(){return 2;}"
                  "int main(){Guard guard{};mut Frame frame{};Frame* pointer=&frame;unsafe{")
        bounds_cases = [(name, prefix + operation + "}}") for name, operation in (
            ("read", "return pointer->slots[outside()];"),
            ("write", "pointer->slots[outside()]=5;return 0;"),
            ("address", "int* element=&pointer->slots[outside()];return *element;"),
            ("comma-address", "int* element=&(0,pointer->slots[outside()]);return *element;"),
            ("element-reference", "mut int& element=frame.slots[outside()];element=3;return element;"),
        )]
        bounds_cases.append(("nested-address", prefix.replace(
            "slots[2]={1,2}", "slots[2][2]={{1,2},{3,4}}") +
            "int* element=&pointer->slots[outside()][0];return *element;}}"))
        bounds_cases.append(("lambda-address", prefix.replace("unsafe{", "") +
            "auto check=[]()->int{mut int values[2]={1,2};int* element=&values[outside()];"
            "unsafe{return *element;}};return check();}"))
        for name, source in bounds_cases:
            path = scratch / f"bounds-{name}.gti"
            path.write_text(source)
            for standard in ("c++20", "c++23"):
                for optimization in ("O0", "O1", "O3"):
                    executable = scratch / f"bounds-{name}-{standard}-{optimization}"
                    run([str(compiler), str(path), "--std", standard, f"-{optimization}",
                         "--progress", "never", "-o", str(executable)])
                    failed = run([str(executable)], expected=70)
                    assert "runtime error[GTI-R" in failed.stderr and "index_out_of_bounds" in failed.stderr, failed
                    assert failed.stdout == "guard dropped\n", failed
        # Each nested base selection completes before the next index begins.
        # Merely checking all dimensions at the final address/load/store runs
        # column() after an already-failing row and reports the wrong origin.
        for name, operation in (
            ("address", "int* element = &values\n    [row()]\n    [column()];\n  unsafe { return *element; }"),
            ("read", "return values\n    [row()]\n    [column()];"),
            ("write", "values\n    [row()]\n    [column()] = 9;\n  return 0;"),
            ("reference", "int& element = values\n    [row()]\n    [column()];\n  return element;"),
            ("grouped-address", "int* element = &(0, values\n    [row()])\n    [column()];\n  unsafe { return *element; }"),
        ):
            source = (
                'class Guard { public: Guard() {} ~Guard() { std::println("guard dropped"); } };\n'
                'int row() { std::println("row"); return 2; }\n'
                'int column() { std::println("column"); return 0; }\n'
                'int main() {\n  Guard guard{};\n'
                '  mut int values[2][2] = {{1, 2}, {3, 4}};\n  ' + operation + '\n}\n'
            )
            path = scratch / f"ordered-{name}.gti"
            path.write_text(source)
            failed_line = next(index + 1 for index, line in enumerate(source.splitlines())
                               if "[row()]" in line)
            for standard in ("c++20", "c++23"):
                for optimization in ("O0", "O2", "O3"):
                    executable = scratch / f"ordered-{name}-{standard}-{optimization}"
                    run([str(compiler), str(path), "--std", standard, f"-{optimization}",
                         "--progress", "never", "-o", str(executable)])
                    failed = run([str(executable)], expected=70)
                    assert failed.stdout == "row\nguard dropped\n", failed
                    assert failed.stderr == (
                        f"{path.name}:{failed_line}: runtime error[GTI-R0007]: "
                        "index_out_of_bounds in fixed_array\n"
                    ), failed
        for name, declaration, operation, cleanup in (
            ("outer-index", "mut int values[2][2] = {{1, 2}, {3, 4}};",
             "int* element = &values\n    [row()]\n    [column()];\n  unsafe { return *element; }", ""),
            ("middle-index", "mut int values[2][2][2] = {{{1, 2}, {3, 4}}, {{5, 6}, {7, 8}}};",
             "int* element = &values\n    [row()]\n    [column()]\n    [depth()];\n  unsafe { return *element; }", ""),
            ("owning-prefix", "Resource values[2][2] = {{Resource(1), Resource(2)}, {Resource(3), Resource(4)}};",
             "return values\n    [row()]\n    [column()].get();", "4\n3\n2\n1\n"),
            ("reference-argument", "Resource values[2][2] = {{Resource(1), Resource(2)}, {Resource(3), Resource(4)}};",
             "return read_arg(values\n    [row()]\n    [column()], depth());", "4\n3\n2\n1\n"),
            ("copied-argument", "Plain values[2][2] = {{Plain(1), Plain(2)}, {Plain(3), Plain(4)}};",
             "return copy_arg(values\n    [row()]\n    [column()], depth());", ""),
        ):
            source = (
                'class Guard { public: Guard() {} ~Guard() { std::println("guard dropped"); } };\n'
                'class Resource { int id; public: Resource(int value) : id(value) {}\n'
                'Resource(Resource& other) = delete; ~Resource() { std::println(this.id); }\n'
                'int get() { return this.id; } };\n'
                'class Plain { int id; public: Plain(int value) : id(value) {} int get() { return this.id; } };\n'
                'int read_arg(Resource& value, int later) { return value.get() + later; }\n'
                'int copy_arg(Plain value, int later) { return value.get() + later; }\n'
                'int row() { std::println("row"); return 1; }\n'
                'int column() { std::println("column"); return 2; }\n'
                'int depth() { std::println("depth"); return 0; }\n'
                'int main() {\n  Guard guard{};\n  ' + declaration + '\n  ' + operation + '\n}\n'
            )
            path = scratch / f"ordered-{name}.gti"
            path.write_text(source)
            failed_line = next(index + 1 for index, line in enumerate(source.splitlines())
                               if "[column()]" in line)
            for standard in ("c++20", "c++23"):
                for optimization in ("O0", "O2", "O3"):
                    executable = scratch / f"ordered-{name}-{standard}-{optimization}"
                    run([str(compiler), str(path), "--std", standard, f"-{optimization}",
                         "--progress", "never", "-o", str(executable)])
                    failed = run([str(executable)], expected=70)
                    assert failed.stdout == "row\ncolumn\n" + cleanup + "guard dropped\n", failed
                    assert failed.stderr == (
                        f"{path.name}:{failed_line}: runtime error[GTI-R0007]: "
                        "index_out_of_bounds in fixed_array\n"
                    ), failed
        for name, source in (
            ("unsafe-required", "struct F{int values[2]={1,2};};int main(){F f{};const F* p=&f;return p->values[0];}"),
            ("const-write", "struct F{mut int values[2]={1,2};};int main(){F f{};const F* p=&f;unsafe{p->values[0]=7;}return 0;}"),
            ("binding-readonly", "struct V{mut int n;V(int n):n(n){}};int main(){V values[2]={V(1),V(2)};auto[first,second]=values;first.n=3;return 0;}"),
            ("binding-arity", "struct V{int n;V(int n):n(n){}};int main(){V values[2]={V(1),V(2)};auto[first]=values;return 0;}"),
            ("binding-copy-owner", "#include <std/string>\n#include <std/utility>\nclass V{public:std::string n;V(std::string n):n(std::move(n)){}};int main(){V values[2]={V(std::string(\"one\")),V(std::string(\"two\"))};auto[first,second]=values;return 0;}"),
            ("element-reference-immutable", "int main(){int values[2]={1,2};mut int& element=values[0];element=3;return 0;}"),
            ("element-reference-overlap", "int main(){mut int values[2]={1,2};mut int& element=values[0];int& alias=values[0];element=3;return alias;}"),
            ("element-reference-write-while-live", "int main(){mut int values[2]={1,2};mut int& element=values[0];values[0]=5;return element;}"),
            ("element-reference-dynamic-overlap", "int pick(int n){return n;}int main(){mut int values[2]={1,2};mut int& first=values[pick(0)];mut int& second=values[pick(1)];first=3;second=4;return 0;}"),
            ("binding-moved-source", "#include <std/utility>\nclass V{public:int n;V(int n):n(n){}};int main(){V values[2]={V(1),V(2)};auto[first,second]=std::move(values);return values[0].n;}"),
        ):
            path = scratch / f"bad-{name}.gti"
            path.write_text(source)
            rejected = run([str(compiler), str(path), "--progress", "never", "--emit-cpp",
                            "-o", str(scratch / "bad.cpp")], expected=65)
            assert "error[GTI-S" in rejected.stderr and "GTI-B0001" not in rejected.stderr, rejected
    print(f"aggregate projections: {len(sources) * 6} native successes, "
          f"{len(bounds_cases) * 6} checked bounds failures, "
          "60 ordered nested failures with exact origins and cleanup, eight sanitizers "
          "and ten semantic negatives passed")


if __name__ == "__main__":
    main()
