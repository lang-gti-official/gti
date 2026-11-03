#!/usr/bin/env python3
"""Explicit and inferred borrows have identical results and failure cleanup."""

import pathlib
import re
import subprocess
import sys
import tempfile
from backend_modes import native_modes


def main():
    compiler = pathlib.Path(sys.argv[1]).resolve()
    root = pathlib.Path(__file__).resolve().parent.parent
    original = (root / "tests/fixtures/borrowed_expected_extraction.gti").read_text()
    inferred, substitutions = re.subn(r"Resource& (\w+) =", r"auto& \1 =", original)
    assert substitutions >= 5, substitutions
    expected = [
        (0, "1\n2\n7\n1\n2\n7\n1\n80\n1\n80\n8\n"),
        (0, "1\n80\n3\n9\n7\n"),
        (0, "1\n80\n3\n9\n7\n"),
        (70, "1\n2\n9\n90\n7\n"),
        (70, "1\n2\n9\n90\n7\n"),
        (70, "1\n2\n9\n90\n7\n"),
    ]
    with tempfile.TemporaryDirectory(prefix="gti-local-reference-") as temporary:
        directory = pathlib.Path(temporary)
        for mode, flags, _native in native_modes():
            for spelling, source in (("explicit", original), ("inferred", inferred)):
                path = directory / f"{spelling}.gti"
                path.write_text(source)
                binary = directory / f"{spelling}-{mode}"
                command = [str(compiler), str(path), *flags, "-o", str(binary)]
                result = subprocess.run(command, capture_output=True, text=True, timeout=120)
                assert result.returncode == 0, (command, result)
                for count, (status, output) in enumerate(expected):
                    result = subprocess.run([str(binary), *(["case"] * count)],
                                            capture_output=True, text=True, timeout=30)
                    assert (result.returncode, result.stdout) == (status, output), result
                    if status:
                        assert "GTI-R0017" in result.stderr and "required borrowed resource" in result.stderr, result
                        fragment = ('lookup(owner, true).expect' if count == 3 else
                                    'alias = std::move(carrier).expect' if count == 4 else
                                    'alias = required(owner, true)')
                        line = next(i + 1 for i, text in enumerate(source.splitlines()) if fragment in text)
                        assert f"{path.name}:{line}:" in result.stderr, result
                    else:
                        assert result.stderr == "", result
            example = root / "examples/131-inferred-references.gti"
            binary = directory / "example"
            result = subprocess.run([str(compiler), str(example), *flags, "-o", str(binary)],
                                    capture_output=True, text=True, timeout=120)
            assert result.returncode == 0, result
            result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=30)
            assert (result.returncode, result.stdout, result.stderr) == (0, "read\nwrite\n", ""), result
            fixture = root / "tests/fixtures/local_reference_lambdas.gti"
            binary = directory / "lambdas"
            result = subprocess.run([str(compiler), str(fixture), *flags, "-o", str(binary)],
                                    capture_output=True, text=True, timeout=120)
            assert result.returncode == 0, result
            for args, status, output in (([], 0, "5\n7\n9\n90\n"),
                                         (["fail"], 70, "5\n7\n90\n")):
                result = subprocess.run([str(binary), *args], capture_output=True,
                                        text=True, timeout=30)
                assert (result.returncode, result.stdout) == (status, output), result
                if status:
                    assert "GTI-R0002" in result.stderr and "division_by_zero" in result.stderr, result
                    assert "local_reference_lambdas.gti:27:" in result.stderr, result
                else:
                    assert result.stderr == "", result
            print(f"local references: explicit/inferred {mode}, cleanup and JSON", flush=True)


if __name__ == "__main__":
    main()
