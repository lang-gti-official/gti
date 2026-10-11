#!/usr/bin/env python3
"""Install and relocate real tools; verify startup, compilation, LSP and bindgen."""

import json
import os
import pathlib
import subprocess
import sys
import tempfile


def run(arguments, environment):
    result = subprocess.run([str(argument) for argument in arguments], env=environment,
                            capture_output=True, text=True, timeout=180)
    if result.returncode:
        raise AssertionError(f"{arguments}:\n{result.stdout}\n{result.stderr}")
    return result.stdout


def main():
    build = pathlib.Path(sys.argv[1]).resolve()
    environment = {name: value for name, value in os.environ.items()
                   if not name.startswith(("DYLD_", "GTI_"))}
    with tempfile.TemporaryDirectory(prefix="gti-relocated-runtime-") as directory:
        root = pathlib.Path(directory)
        stage = root / "original-stage"
        run(["cmake", "--install", build, "--prefix", stage,
             "--component", "gti_toolchain"], environment)
        # A literal space also checks subprocess/path quoting in install hooks.
        relocated = root / "relocated toolchain"
        stage.rename(relocated)
        cli = relocated / "bin/gti"
        lsp = relocated / "bin/gti_lsp"
        assert "gti" in run([cli, "--version"], environment)
        assert "gti" in run([lsp, "--version"], environment)
        commands = root / "terminal commands"
        commands.mkdir()
        for tool in ("gti", "gti_lsp"):
            alias = commands / tool
            alias.symlink_to(relocated / "bin" / tool)
            assert tool in run([alias, "--version"], environment)
        run([cli, "--help"], environment)
        manifest = json.loads((relocated / "share/gti/host-runtime.json").read_text())
        for file in [cli, lsp, *(relocated / name for name in manifest["libraries"])]:
            output = run(["otool", "-L", file], environment).splitlines()[1:]
            for line in output:
                name = line.strip().split(" (compatibility version ", 1)[0]
                # A dylib's install ID is not a dependency.
                if name.startswith("@rpath/") and name == "@rpath/" + file.name:
                    continue
                assert name.startswith(("@loader_path/", "/usr/lib/", "/System/Library/")), name
                if name.startswith("@loader_path/"):
                    dependency = (file.parent / name[len("@loader_path/"):]).resolve(strict=True)
                    assert dependency.is_relative_to(relocated.resolve()), dependency
            run(["codesign", "--verify", file], environment)
        source = root / "consumer.gti"
        source.write_text('#include <std/string>\nextern "C" { int32_t getpid(); }\n'
                          'int main() { std::string text{"portable installed GTI"};\n'
                          'std::println(text.view()); if (getpid() <= 0) { return 1; }\n'
                          'return 0; }\n')
        executable = root / "consumer"
        run([cli, source, "-o", executable], environment)
        assert "portable installed GTI" in run([executable], environment)
        # Resolve an installed interface and its republished legacy unit after
        # relocation, without a source-tree stdlib or an environment override.
        assert (relocated / "share/gti/stdlib/modules/string.gti").is_file()
        source.write_text(source.read_text().replace(
            '#include <std/string>', 'import std.string;'))
        run([cli, source, "-o", executable], environment)
        assert "portable installed GTI" in run([executable], environment)
        # The owned environment API and its module must work without source
        # paths/overrides after relocation, using only the packaged runtime.
        source.write_text('import std.environment;\n#include <std/string>\n'
                          'int main() { auto result = std::environment::get("GTI_INSTALLED_ENVIRONMENT");\n'
                          'if (!result) { return 1; }\n'
                          'auto value = std::move(result).expect("installed environment lookup");\n'
                          'if (!value || value.value().view() != "relocated") { return 2; }\n'
                          'std::println("owned installed environment"); return 0; }\n')
        run([cli, source, "-O3", "-o", executable], environment)
        assert run([executable], dict(environment, GTI_INSTALLED_ENVIRONMENT="relocated")) == "owned installed environment\n"
        header = root / "consumer.h"
        header.write_text("#include <stdint.h>\nint32_t native_add(int32_t a, int32_t b);\n")
        bindings = root / "bindings.gti"
        run([cli, "bindgen", header, "--namespace", "native", "-o", bindings], environment)
        assert "native_add" in bindings.read_text()
    print("Relocated installed macOS CLI/LSP, runtime closure, native compilation and bindgen passed")


if __name__ == "__main__":
    main()
