#!/usr/bin/env python3
"""Focused generated module-name contract; no native link or broad CLI suite."""
import os
import pathlib
import subprocess
import sys
import tempfile

compiler = sys.argv[1]
with tempfile.TemporaryDirectory(prefix="gti-bindgen-module-") as temporary:
    root = pathlib.Path(temporary)
    header = root / "native-api.h"
    header.write_text("typedef int NativeValue;\n", encoding="utf-8")
    def run(*arguments, success=True):
        result = subprocess.run([compiler, "bindgen", str(header), *arguments], text=True, capture_output=True)
        assert (result.returncode == 0) == success, result.stderr
        return result
    for filename, expected in [("first.gti", "first"), ("second-api.gti", "second_api"), ("class.gti", "bindings_class")]:
        output = root / filename
        run("-o", str(output))
        assert f"export module {expected};" in output.read_text(encoding="utf-8")
    assert "export module native_api;" in run().stdout
    assert "export module native.http;" in run("--module", "native.http").stdout
    run("--namespace", "first", "-o", str(root / "first.gti"))
    run("--namespace", "second", "-o", str(root / "second.gti"))
    consumer = root / "main.gti"
    consumer.write_text('import "first.gti";\nimport "second.gti";\n'
                        'int main() { return int(first::NativeValue(20) + second::NativeValue(22)); }\n',
                        encoding="utf-8")
    environment = dict(os.environ)
    environment["GTI_STDLIB_PATH"] = str(pathlib.Path(__file__).resolve().parent.parent / "stdlib")
    checked = subprocess.run([compiler, str(consumer), "--emit-mir", "-o", str(root / "main.mir")],
                             text=True, capture_output=True, env=environment)
    assert checked.returncode == 0, checked.stderr
    for invalid in ("class", "__gti_hidden", "api.__gti_hidden", "name.", "a..b", "a b", "a/*x*/.b"):
        assert "--module requires" in run("--module", invalid, success=False).stderr
print("bindgen module CLI checks passed")
