#!/usr/bin/env python3
"""Constructor-local packs consume owners once and unwind completed fields."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile


def run(command, expected=0):
    result = subprocess.run(command, capture_output=True, text=True, timeout=120)
    if result.returncode != expected:
        raise AssertionError(f"{command}: {result.returncode}\n{result.stdout}\n{result.stderr}")
    return result


def main():
    compiler, repository = Path(sys.argv[1]).resolve(), Path(sys.argv[2]).resolve()
    os.environ["GTI_STDLIB_PATH"] = str(repository / "stdlib")
    strong = repository / "tests/fixtures/constructor_pack_field_forwarding.gti"
    sources = [
        (repository / "tests/coverage/reproducers/a55-ctor-pack-move-only-field-init.gti", ""),
        (repository / "tests/coverage/reproducers/a55-list-emplace-move-only-argument.gti", ""),
        # Deque's generated destruction drains its physical owner directory
        # in reverse slot order, rather than calling logical pop_back().
        (strong, "14\n13\n4\n3\n6\n5\n16\n15\n18\n17\n8\n7\n2\n1\n9\n"),
        (repository / "examples/089-library-workflow.gti", "finished: build\nfinished: test\n"),
    ]
    with tempfile.TemporaryDirectory(prefix="gti-constructor-pack-") as temporary:
        scratch = Path(temporary)
        for standard in ("c++20", "c++23"):
            for level in ("O0", "O1", "O3"):
                for source, output in sources:
                    executable = scratch / f"{source.stem}-{standard}-{level}"
                    run([str(compiler), str(source), "--std", standard, f"-{level}",
                         "--progress", "never", "-o", str(executable),
                         "--", "-fno-elide-constructors"])
                    result = run([str(executable)])
                    assert result.stdout == output and result.stderr == "", result
                    if source == strong:
                        result = run([str(executable), "fail"], 70)
                        assert result.stdout == "2\n1\n9\n", result
                        assert "constructor_pack_field_forwarding.gti:31: runtime error[GTI-R0002]" in result.stderr, result
        executable = scratch / "sanitized"
        run([str(compiler), str(strong), "-O1", "--progress", "never", "-o", str(executable),
             "--", "-fsanitize=address,undefined", "-fno-elide-constructors"])
        run([str(executable)])
        result = run([str(executable), "fail"], 70)
        assert result.stdout == "2\n1\n9\n" and "GTI-R0002" in result.stderr, result
    print("constructor pack fields: 24 native cases, six failures and sanitizer passed")


if __name__ == "__main__":
    main()
