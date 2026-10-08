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
                "class_array_arguments", "array_element_references")] + [
        repository / f"tests/coverage/reproducers/{name}.gti"
        for name in ("a39-ctor-address-of-struct-field", "a40-raw-pointer-fixed-array-field-index",
                     "a40-c-abi-record-pointer-array-field",
                     "a51-binding-over-class-array", "a51-binding-over-class-array-invalid-cpp")
    ] + [repository / f"examples/{name}.gti" for name in (
        "32-structured-bindings", "12-fixed-arrays", "111-final-address-storage")]
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
                    expected_output = ("structured bindings worked\n" if source.name.startswith("32-") else
                                       "fixed arrays keep their bounds\n" if source.name.startswith("12-") else "")
                    assert executed.stdout == expected_output and executed.stderr == "", executed
        for source in sources[:6]:
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
    print("aggregate projections: 84 native successes, 42 checked bounds failures with cleanup, six sanitizers and ten semantic negatives passed")


if __name__ == "__main__":
    main()
