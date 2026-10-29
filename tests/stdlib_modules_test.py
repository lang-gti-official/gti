#!/usr/bin/env python3
"""Every public standard-library unit is importable as exactly one module."""

import os
import pathlib
import subprocess
import sys
import tempfile

from case_jobs import configured_jobs, run_cases


def main():
    compiler = pathlib.Path(sys.argv[1]).resolve()
    stdlib = pathlib.Path(sys.argv[2]).resolve() / "stdlib"
    units = sorted(path.stem for path in (stdlib / "std").glob("*.gti"))
    modules = units
    assert not (stdlib / "modules").exists(), "obsolete wrapper tree remains"
    for name in modules:
        text = (stdlib / "std" / f"{name}.gti").read_text(encoding="utf-8")
        assert f"export module {name};" in text, f"std.{name} lacks its module declaration"
        assert "#include" not in text, f"std.{name} uses a removed directive"
    portable = sorted(set(units) - {"environment", "process", "tcp", "detail_iterator",
                                    "detail_panic", "experimental_optional"})
    aggregate = (stdlib / "std.gti").read_text(encoding="utf-8")
    assert [line for line in aggregate.splitlines() if line.startswith("export import ")] == [
        f"export import std.{name};" for name in portable], "portable aggregate inventory drift"

    environment = dict(os.environ, GTI_STDLIB_PATH=str(stdlib))
    with tempfile.TemporaryDirectory(prefix="gti-stdlib-modules-") as temporary:
        root = pathlib.Path(temporary)

        def check(name):
            source = root / f"{name}.gti"
            source.write_text(f"import std.{name};\nint main() {{ return 0; }}\n",
                              encoding="utf-8")
            result = subprocess.run(
                [str(compiler), str(source), "--emit-mir", "-o", os.devnull],
                capture_output=True, text=True, env=environment, timeout=300)
            assert result.returncode == 0, result.stdout + result.stderr

        run_cases([(name, name) for name in modules], check, configured_jobs())
        source = root / "aggregate.gti"
        source.write_text("import std;\nimport std.vector;\nimport std.string;\n"
                          "int main() { mut std::vector<int> values{}; values.push_back(42); "
                          "return values[std::size_t(0)] - 42; }\n")
        result = subprocess.run(
            [str(compiler), str(source), "--emit-mir", "-o", os.devnull],
            capture_output=True, text=True, env=environment, timeout=300)
        assert result.returncode == 0, result.stdout + result.stderr
    print(f"standard-library modules: {len(modules)} interfaces import and lower")


if __name__ == "__main__":
    main()
