#!/usr/bin/env python3
"""Selected base construction and rollback obey one verified MIR schedule."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile
from harness_timeouts import scaled


def run(command, expected=0):
    result = subprocess.run(command, capture_output=True, text=True, timeout=scaled(120))
    assert result.returncode == expected, (command, result)
    return result


def main():
    compiler, repository = Path(sys.argv[1]).resolve(), Path(sys.argv[2]).resolve()
    os.environ["GTI_STDLIB_PATH"] = str(repository / "stdlib")
    reproducer = repository / "tests/coverage/reproducers"
    strong = repository / "tests/fixtures/base_constructor_stages.gti"
    effectful = repository / "tests/fixtures/effectful_base_arguments.gti"
    checked = repository / "tests/fixtures/checked_base_arguments.gti"
    success = "1\n5\n2\n3\n6\n16\n13\n12\n15\n11\n"
    sources = [
        (reproducer / "a45-base-state-rollback-unsupported.gti", "2\n"),
        (reproducer / "a45-base-plus-string-field-unsupported.gti", ""),
        (reproducer / "a45-literal-base-argument.gti", ""),
        (reproducer / "a45-derived-destructor-explicit-base.gti", "1\n-2\n"),
        (reproducer / "a45-derived-destructor-generated-ctor.gti", "1\n2\n"),
        (strong, success),
        (checked, "10\n20\n10\n"),
        (effectful, "9\n1\n2\n30\n40\n50\n150\n140\n130\n109\n"),
        (repository / "tests/fixtures/generated_abstract_base.gti", ""),
        (repository / "examples/005-objects.gti", "objects use explicit construction\n"),
    ]
    failures = [
        (1, "1\n5\n11\n", "GTI-R0002", 27),
        (2, "1\n5\n2\n3\n6\n13\n12\n15\n11\n", "GTI-R0002", 48),
        (3, "1\n5\n2\n3\n12\n15\n11\n", "GTI-R0002", 13),
        (4, "1\n5\n2\n3\n6\n13\n12\n15\n", "GTI-R0014", 34),
    ]

    def verify(executable, expected, source):
        result = run([str(executable)])
        assert result.stdout == expected and result.stderr == "", result
        if source == checked:
            result = run([str(executable), "overflow"], 70)
            assert result.stdout == "", result
            assert "checked_base_arguments.gti:12: runtime error[GTI-R0001]" in result.stderr, result
            return
        if source == effectful:
            for phase, output, line in (
                (1, "9\n1\n2\n109\n", 11),
                (2, "9\n1\n2\n30\n109\n", 28),
                (3, "9\n1\n2\n30\n40\n50\n140\n130\n109\n", 42),
            ):
                result = run([str(executable)] + ["phase"] * phase, 70)
                assert result.stdout == output, result
                assert f"effectful_base_arguments.gti:{line}: runtime error[GTI-R0002]" in result.stderr, result
            return
        if source != strong:
            return
        for phase, output, code, line in failures:
            result = run([str(executable)] + ["phase"] * phase, 70)
            assert result.stdout == output, result
            assert f"base_constructor_stages.gti:{line}: runtime error[{code}]" in result.stderr, result
            if code == "GTI-R0014":
                assert "primary [GTI-R0002] at base_constructor_stages.gti:48" in result.stderr, result
                assert "secondary [GTI-R0002]" in result.stderr, result

    with tempfile.TemporaryDirectory(prefix="gti-base-stage-") as temporary:
        scratch = Path(temporary)
        for standard in ("c++20", "c++23"):
            for level in ("O0", "O1", "O3"):
                for source, expected in sources:
                    executable = scratch / f"{source.stem}-{standard}-{level}"
                    run([str(compiler), str(source), "--std", standard, f"-{level}",
                         "--progress", "never", "-o", str(executable),
                         "--", "-fno-elide-constructors"])
                    verify(executable, expected, source)
        executable = scratch / "sanitized"
        run([str(compiler), str(strong), "-O1", "--progress", "never", "-o", str(executable),
             "--", "-fsanitize=address,undefined", "-fno-elide-constructors"])
        verify(executable, success, strong)
        run([str(compiler), str(effectful), "-O1", "--progress", "never", "-o", str(executable),
             "--", "-fsanitize=address,undefined", "-fno-elide-constructors"])
        verify(executable, sources[7][1], effectful)
    print("base constructor stage: 60 native cases, 48 failures and 9 sanitizer runs passed")


if __name__ == "__main__":
    main()
