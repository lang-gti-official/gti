#!/usr/bin/env python3
"""Class field value transport retains GTI mutation, movement and defaults."""
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
    sources = [repository / "tests/fixtures/class_field_values.gti",
               repository / "tests/fixtures/owning_class_field_values.gti",
               repository / "examples/023-direct-initialization.gti"] + [
        repository / f"tests/coverage/reproducers/{name}.gti"
        for name in ("a01-class-value-into-field", "a01-call-result-into-field",
                     "a01-field-assign-from-call-result-method",
                     "a14-generated-ctor-field-in-user-ctor",
                     "a50-designated-omitted-brace-default")
    ]
    with tempfile.TemporaryDirectory(prefix="gti-class-fields-") as directory:
        scratch = Path(directory)
        for standard in ("c++20", "c++23"):
            for optimization in ("O0", "O1", "O3"):
                for source in sources:
                    executable = scratch / f"{source.stem}-{standard}-{optimization}"
                    run([str(compiler), str(source), "--std", standard,
                         f"-{optimization}", "--progress", "never", "-o", str(executable),
                         "--", "-fno-elide-constructors"])
                    executed = run([str(executable)])
                    assert executed.stdout == "" and executed.stderr == "", executed
        for source in sources[:2]:
            sanitized = scratch / f"{source.stem}-sanitized"
            run([str(compiler), str(source), "-O1", "--progress", "never", "-o", str(sanitized),
                 "--", "-fsanitize=address,undefined", "-fno-elide-constructors"])
            run([str(sanitized)])
        for name, source in (
            ("immutable", "struct V{int n=1; public: V() = default; };struct H{V v=V(); public: H() = default; };int main(){H h{};V v{};h.v=v;return 0;}"),
            ("moved", "#include <std/utility>\nstruct V{int n=1; public: V() = default; };struct H{mut V v=V(); public: H() = default; };int main(){mut H h{};V v{};h.v=std::move(v);h.v=v;return 0;}"),
            ("owning-copy", sources[1].read_text().replace(
                "owner.value = std::move(replacement);", "owner.value = replacement;")),
            ("unknown-designator", "struct V{int n=1; public: V() = default; };struct H{V v{}; public: H() = default; };int main(){H h={.missing=1};return 0;}"),
        ):
            path = scratch / f"{name}.gti"
            path.write_text(source)
            rejected = run([str(compiler), str(path), "--progress", "never", "--emit-cpp",
                            "-o", str(scratch / "bad.cpp")], expected=65)
            assert "error[GTI-S" in rejected.stderr and "GTI-B0001" not in rejected.stderr, rejected
            if name == "owning-copy":
                assert "GTI-S2003" in rejected.stderr and "Cannot assign a value of type 'OwnedValue'" in rejected.stderr, rejected
    print("class fields: 48 native cases, two ASan/UBSan runs and four semantic negatives passed")


if __name__ == "__main__":
    main()
