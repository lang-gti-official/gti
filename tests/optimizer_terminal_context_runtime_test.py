#!/usr/bin/env python3
"""Terminal alternatives preserve successful proofs and real cleanup/failures."""

from pathlib import Path
import os
import subprocess
import sys
import tempfile


def main():
    compiler = Path(sys.argv[1]).resolve()
    fixture = Path(__file__).parent / "fixtures/optimizer_terminal_context.gti"
    source = fixture.read_text()
    lines = source.splitlines()
    panic_line = next(i for i, line in enumerate(lines, 1) if "std::panic" in line)
    bad_start = next(i for i, line in enumerate(lines, 1) if "int bad_surviving_value" in line)
    overflow_line = next(i for i, line in enumerate(lines, 1)
                         if i > bad_start and "return value + 1" in line)
    cases = [
        ("success", source, (0, "", "")),
        ("function", source.replace("after_terminal_branch(7, false)",
                                    "after_terminal_branch(7, true)"),
         (70, "42\n", f'function.gti:{panic_line}: runtime error[GTI-R0017]: panic: "terminal context"\n')),
        ("constructor", source.replace("record{7, false}", "record{7, true}"),
         (70, "42\n", f'constructor.gti:{panic_line}: runtime error[GTI-R0017]: panic: "terminal context"\n')),
        ("overflow", source.replace("if (false) return bad_surviving_value", "if (true) return bad_surviving_value"),
         (70, "", f"overflow.gti:{overflow_line}: runtime error[GTI-R0001]: integer_overflow in addition\n")),
    ]
    for call in ("choose_exact", "choose_equal"):
        site = next(i for i, line in enumerate(lines, 1) if f"return {call}(0) + 1" in line)
        cases.append((call, source.replace(f"{call}(0)", f"{call}(1)"),
                      (70, "", f"{call}.gti:{site}: runtime error[GTI-R0001]: integer_overflow in addition\n")))
    modes = [(f"{standard}-{level}", ["--std", standard, f"-{level}"])
             for standard in ("c++20", "c++23") for level in ("O0", "O2", "O3")]
    modes.append(("sanitized", ["--std", "c++23", "-O1", "--",
                                "-fsanitize=address,undefined", "-fno-omit-frame-pointer"]))
    environment = dict(os.environ, ASAN_OPTIONS="halt_on_error=1",
                       UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1")
    with tempfile.TemporaryDirectory(prefix="gti-terminal-context-") as directory:
        root = Path(directory)
        for name, flags in modes:
            for case, text, expected in cases:
                path = root / f"{case}.gti"
                path.write_text(text)
                binary = root / f"{case}-{name}"
                built = subprocess.run([str(compiler), str(path), "-o", str(binary), *flags],
                                       capture_output=True, text=True, timeout=120, env=environment)
                assert built.returncode == 0, (name, case, built.stdout, built.stderr)
                result = subprocess.run([str(binary)], capture_output=True, text=True,
                                        timeout=30, env=environment)
                assert (result.returncode, result.stdout, result.stderr) == expected, (name, case, result)
            print(f"Terminal contexts: {name} passed ({len(cases)} cases)", flush=True)


if __name__ == "__main__":
    main()
