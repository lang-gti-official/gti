#!/usr/bin/env python3
"""Classify borrowed text results without hiding C++ representation failures."""
from pathlib import Path
import subprocess
import sys
import tempfile

from backend_modes import cpp_modes, llvm_modes


def run(command):
    return subprocess.run(list(map(str, command)), capture_output=True,
                          text=True, timeout=180, check=False)


def require_success(result):
    assert result.returncode == 0, (result.returncode, result.stdout, result.stderr)


def main():
    compiler = Path(sys.argv[1]).resolve()
    repository = Path(__file__).resolve().parents[1]
    with tempfile.TemporaryDirectory(prefix="gti-cpp-text-borrow-") as temporary:
        root = Path(temporary)
        borrowed = root / "borrowed.gti"
        borrowed.write_text('''import std.string;
struct Label { std::string_view text; Label(std::string_view v) : text(v) {} };
std::string_view echo<T>(T ignored, std::string_view value) { return value; }
bool observe<T>(T value, Label& label) { return echo<T>(value, label.text) == "bird"; }
int main() { Label label{"bird"}; return observe<int>(7, label) ? 0 : 1; }
''')
        original = repository / "examples/procedural-html.gti"
        for source, declaration_line in ((borrowed, 4), (original, 12)):
            output = root / "preserved.cpp"
            output.write_text("existing artifact\n")
            rejected = run([compiler, source, "--emit-cpp", "-O0", "-o", output])
            assert rejected.returncode == 65, (source, rejected)
            assert "error[GTI-B0003]" in rejected.stderr, rejected.stderr
            assert "borrow-dependent text call results" in rejected.stderr, rejected.stderr
            assert "LLVM backend" in rejected.stderr, rejected.stderr
            assert "GTI-B0001" not in rejected.stderr, rejected.stderr
            assert f"{source}:{declaration_line}:" in rejected.stderr, rejected.stderr
            assert output.read_text() == "existing artifact\n"
            for mode, flags, _native in llvm_modes():
                binary = root / f"{source.stem}-{mode}"
                require_success(run([compiler, source, *flags, "-o", binary]))
                result = run([binary])
                require_success(result)
                assert not result.stdout and not result.stderr, result

        # A text return with no retained owner and an ordinary reference result
        # retain their existing C++ routes. Generic spelling alone is no limit.
        supported = root / "supported.gti"
        supported.write_text('''import std.string;
std::string_view text<T>(T ignored) { return "bird"; }
int& reference(int& value) { return value; }
int main() { int value = 7; return text<int>(value) == "bird" && reference(value) == 7 ? 0 : 1; }
''')
        for mode, flags, _native in cpp_modes("cpp"):
            binary = root / f"supported-{mode}"
            require_success(run([compiler, supported, *flags, "-o", binary]))
            require_success(run([binary]))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
