#!/usr/bin/env python3
"""Every public standard-library unit is importable as exactly one module."""

import json
import os
import pathlib
import subprocess
import sys
import tempfile

from case_jobs import configured_jobs, run_cases


def main():
    compiler = pathlib.Path(sys.argv[1]).resolve()
    stdlib = pathlib.Path(sys.argv[2]).resolve() / "stdlib"
    mapping = json.loads((stdlib / "module-map.json").read_text(encoding="utf-8"))
    modules = mapping["modules"]
    assert mapping["default"] == "std" and modules["std"] == "std.gti"
    # Public files live outside detail/. Check both directions so dropping a
    # registration cannot silently reduce this test's coverage.
    public_files = {stdlib / "std.gti", *(path for path in (stdlib / "std").rglob("*.gti")
                    if "detail" not in path.relative_to(stdlib / "std").parts)}
    mapped_files = {(stdlib / relative).resolve() for relative in modules.values()}
    assert mapped_files == public_files, "public source/module-map inventory drift"
    assert not (stdlib / "modules").exists(), "obsolete wrapper tree remains"
    for name, relative in modules.items():
        source = (stdlib / relative).resolve()
        assert source.is_relative_to(stdlib) and source.is_file(), (name, relative)
        text = source.read_text(encoding="utf-8")
        assert f"export module {name};" in text, f"std.{name} lacks its module declaration"
        assert "#include" not in text, f"std.{name} uses a removed directive"
    portable = sorted(name for name in modules
                      if name not in {"std", "environment", "process", "tcp"}
                      and not name.startswith("experimental."))
    aggregate = (stdlib / "std.gti").read_text(encoding="utf-8")
    assert [line for line in aggregate.splitlines() if line.startswith("export import ")] == [
        f"export import std.{name};" for name in portable], "portable aggregate inventory drift"

    environment = dict(os.environ, GTI_STDLIB_PATH=str(stdlib))
    with tempfile.TemporaryDirectory(prefix="gti-stdlib-modules-") as temporary:
        root = pathlib.Path(temporary)

        def check(name):
            source = root / f"{name}.gti"
            spelling = "std" if name == mapping["default"] else f"std.{name}"
            source.write_text(f"import {spelling};\nint main() {{ return 0; }}\n",
                              encoding="utf-8")
            result = subprocess.run(
                [str(compiler), str(source), "--emit-mir", "-o", os.devnull],
                capture_output=True, text=True, env=environment, timeout=300)
            assert result.returncode == 0, result.stdout + result.stderr

        run_cases([(name, name) for name in sorted(modules)], check, configured_jobs())
        source = root / "aggregate.gti"
        source.write_text("import std;\n"
                          "int main() { mut std::vector<int> values{}; values.push_back(42); "
                          'std::string text{"ok"}; '
                          "return values[std::size_t(0)] == 42 && text.size() == std::size_t(2) ? 0 : 1; }\n")
        result = subprocess.run(
            [str(compiler), str(source), "--emit-mir", "-o", os.devnull],
            capture_output=True, text=True, env=environment, timeout=300)
        assert result.returncode == 0, result.stdout + result.stderr
    print(f"standard-library modules: {len(modules)} interfaces import and lower")


if __name__ == "__main__":
    main()
