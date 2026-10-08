#!/usr/bin/env python3
"""Explicit and inferred borrows have identical results and failure cleanup."""

import pathlib
import re
import subprocess
import sys
import tempfile


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
        for standard in ("20", "23"):
            for level in ("0", "3"):
                for spelling, source in (("explicit", original), ("inferred", inferred)):
                    path = directory / f"{spelling}.gti"
                    path.write_text(source)
                    binary = directory / f"{spelling}-{standard}-{level}"
                    command = [str(compiler), str(path), "--std", f"c++{standard}",
                               f"-O{level}", "-o", str(binary), "--", "-fno-elide-constructors"]
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
                result = subprocess.run([str(compiler), str(example), "--std", f"c++{standard}",
                                         f"-O{level}", "-o", str(binary)],
                                        capture_output=True, text=True, timeout=120)
                assert result.returncode == 0, result
                result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=30)
                assert (result.returncode, result.stdout, result.stderr) == (0, "read\nwrite\n", ""), result
                print(f"local references: explicit/inferred C++{standard}/O{level}, cleanup and JSON", flush=True)


if __name__ == "__main__":
    main()
