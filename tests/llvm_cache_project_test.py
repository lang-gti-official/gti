#!/usr/bin/env python3
"""Real LLVM project/backend/install workflows, without generated C++ fallback."""

import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile


def run(arguments, cwd, expected=0, env=None):
    result = subprocess.run(
        [str(value) for value in arguments], cwd=cwd, env=env,
        text=True, capture_output=True, timeout=90, check=False,
    )
    if result.returncode != expected:
        raise AssertionError(
            f"{arguments}: exit {result.returncode}, expected {expected}\n"
            f"stdout:\n{result.stdout}\nstderr:\n{result.stderr}"
        )
    return result


def main():
    if len(sys.argv) != 2:
        raise SystemExit("usage: llvm_cache_project_test.py /absolute/path/to/gti")
    gti = Path(sys.argv[1]).resolve()
    compiler = shutil.which("c++")
    if compiler is None:
        raise AssertionError("native C++ linker is required")
    environment = os.environ.copy()
    for name in (
        "CPATH", "C_INCLUDE_PATH", "CPLUS_INCLUDE_PATH", "OBJC_INCLUDE_PATH",
        "LIBRARY_PATH", "COMPILER_PATH", "GCC_EXEC_PREFIX", "SDKROOT",
        "DEVELOPER_DIR", "LD_LIBRARY_PATH", "DYLD_LIBRARY_PATH",
        "DYLD_FRAMEWORK_PATH", "DYLD_FALLBACK_FRAMEWORK_PATH", "INCLUDE", "LIB",
        "MACOSX_DEPLOYMENT_TARGET",
    ):
        environment.pop(name, None)

    with tempfile.TemporaryDirectory(prefix="gti-llvm-project-") as temporary:
        root = Path(temporary)
        project = root / "app"
        dependency = root / "dependency"
        for directory in (project / "src", project / "native", dependency / "src"):
            directory.mkdir(parents=True)
        (project / "src/main.gti").write_text(
            '#include "logic.gti"\n#include <dep/value>\n'
            "int main() { return 17 == expected_value() + "
            "dep::offset() ? 0 : 1; }\n", encoding="utf-8",
        )
        source_dependency = project / "src/logic.gti"
        source_dependency.write_text(
            "int expected_value() { return 17; }\n", encoding="utf-8")
        (dependency / "src/value.gti").write_text(
            "namespace dep { int offset() { return 0; } }\n", encoding="utf-8")
        (dependency / "gti.toml").write_text(
            'manifest-version = 1\n[package]\nname = "cache-dependency"\n'
            'version = "1.0.0"\n[package.build-script]\n'
            'command = ["python3", "generate.py"]\ninputs = ["generate.py"]\n',
            encoding="utf-8")
        (dependency / "generate.py").write_text(
            'import os\nfrom pathlib import Path\n'
            'out = Path(os.environ["GTI_BUILD_OUT_DIR"])\n'
            '(out / "generated.c").write_text("int cache_script_marker(void) { return 9; }\\n")\n'
            '(out / "gti-build-output.toml").write_text(\'version = 1\\nc-sources = ["generated.c"]\\n\')\n',
            encoding="utf-8")
        native_header = project / "native/value.h"
        native_header.write_text("#define CACHE_VALUE 17\n", encoding="utf-8")
        (project / "native/value.c").write_text(
            '#include "value.h"\nint cache_native_value(void) { return CACHE_VALUE; }\n',
            encoding="utf-8")
        manifest = (
            'manifest-version = 1\n[package]\nname = "llvm-project"\n'
            'version = "0.1.0"\n[package.native]\nc-sources = ["native/value.c"]\n'
            'c-standard = "c17"\n[dependencies]\ndep = { path = "../dependency" }\n'
            '[build]\nallow-build-scripts = ["cache-dependency@1.0.0"]\n'
            '[targets.app]\nkind = "executable"\nroot = "src/main.gti"\n'
            '[targets.other]\nkind = "executable"\nroot = "src/main.gti"\n'
            '[targets.smoke]\nkind = "test"\nroot = "src/main.gti"\n'
            '[profiles.dev]\nbackend = "llvm"\noptimization = 3\n'
        )
        manifest_path = project / "gti.toml"
        manifest_path.write_text(manifest, encoding="utf-8")
        log = root / "native-commands.jsonl"
        wrapper = root / "cxx-guard"
        wrapper.write_text(
            f"#!{sys.executable}\nimport json, os, sys\n"
            f"with open({str(log)!r}, 'a') as stream: "
            "stream.write(json.dumps(sys.argv[1:]) + '\\n')\n"
            "if any(value.endswith('.gti.cpp') for value in sys.argv[1:]):\n"
            "    raise SystemExit('LLVM invoked generated C++ compilation')\n"
            f"os.execv({compiler!r}, [{compiler!r}] + sys.argv[1:])\n",
            encoding="utf-8",
        )
        wrapper.chmod(0o700)

        def invoke(*arguments, expected=0):
            return run([gti, *arguments], project, expected, environment)

        metadata = json.loads(invoke("metadata").stdout)
        assert metadata["schemaVersion"] == 15
        target = next(value for value in metadata["targets"] if value["name"] == "app")
        dev = next(value for value in target["outputs"] if value["profile"] == "dev")
        assert dev["backend"] == "llvm" and dev["generatedCpp"] == ""
        assert dev["generatedArtifact"].endswith(".gti.o")
        output = Path(dev["executable"])
        first = invoke("build", "app", "--cxx", wrapper, "--verbose")
        assert "gti: cache miss " in first.stderr, first.stderr
        run([output], project, env=environment)
        first_bytes = output.read_bytes()
        symbols = run(["nm", output], project, env=environment)
        assert "cache_native_value" in symbols.stdout, symbols.stdout
        assert "cache_script_marker" in symbols.stdout, symbols.stdout
        output.unlink()
        hit = invoke("build", "app", "--cxx", wrapper, "--verbose")
        assert "gti: cache hit " in hit.stderr, hit.stderr
        assert output.read_bytes() == first_bytes
        invoke("run", "app", "--cxx", wrapper)
        explicit_run = invoke("run", "app", "--backend", "llvm", "--cxx", wrapper,
                              "--verbose")
        assert "gti: cache hit " in explicit_run.stderr, explicit_run.stderr
        invoke("run", "app", "--no-build")
        invoke("test", "smoke", "--backend", "llvm", "--cxx", wrapper)
        invoke("run", "app", "--no-build", "--backend", "llvm", expected=64)
        invoke("build", "app", "--backend", "llvm", "--std", "c++23", expected=65)
        invoke("build", "app", "--backend", "llvm", "--keep-cpp", expected=65)
        invoke("build", "app", "--backend", "unknown", expected=64)
        invoke("run", "app", "--backend", "unknown", expected=64)
        invoke("run", "app", "--backend", expected=64)
        invoke("build", "app", "--backend", "llvm", "--backend", "cpp", expected=64)
        invoke("check", "--backend", "llvm", expected=64)
        invoke("build", "app", "--backend", "llvm", "--emit-mir", expected=64)
        invoke("build", "--all", "--jobs", "2", "--backend", "llvm", "--cxx", wrapper)
        source_dependency.write_text(
            "int expected_value() { return 17; } // new source identity\n",
            encoding="utf-8")
        source_miss = invoke("build", "app", "--cxx", wrapper, "--verbose")
        assert "gti: cache miss " in source_miss.stderr, source_miss.stderr
        native_header.write_text("#define CACHE_VALUE 23\n", encoding="utf-8")
        native_miss = invoke("build", "app", "--cxx", wrapper, "--verbose")
        assert "gti: cache miss " in native_miss.stderr, native_miss.stderr
        run([output], project, env=environment)
        assert output.read_bytes() != first_bytes
        native_header.write_text("#define CACHE_VALUE 17\n", encoding="utf-8")
        invoke("build", "app", "--cxx", wrapper)
        no_cache = invoke("build", "app", "--cxx", wrapper, "--no-cache", "--verbose")
        assert "cache disabled by --no-cache" in no_cache.stderr
        installed_root = root / "programs"
        invoke("install", "--list", "--backend", "llvm", "--root", installed_root,
               expected=64)
        invoke("install", "app", "--profile", "dev", "--backend", "llvm",
               "--cxx", wrapper, "--root", installed_root)
        run([installed_root / "bin/app"], root, env=environment)
        invoke("uninstall", "app", "--root", installed_root)
        assert not (installed_root / "bin/app").exists()
        commands = [json.loads(line) for line in log.read_text().splitlines()]
        links = [command for command in commands if "--version" not in command]
        assert links and all(any(value.endswith(".gti.o") for value in command)
                             for command in links), links
        assert all("-std=c++23" not in command and "-x" not in command
                   for command in links), links
        cpp = invoke("build", "app", "--backend", "cpp", "--verbose")
        assert "gti: cache miss " in cpp.stderr, cpp.stderr
        cpp_hit = invoke("build", "app", "--backend", "cpp", "--verbose")
        assert "gti: cache hit " in cpp_hit.stderr, cpp_hit.stderr
        explicit_run = invoke("run", "app", "--backend", "llvm", "--cxx", wrapper,
                              "--verbose")
        assert "gti: cache hit " in explicit_run.stderr, explicit_run.stderr
        restored = invoke("build", "app", "--backend", "llvm", "--cxx", wrapper,
                          "--verbose")
        assert "gti: cache hit " in restored.stderr, restored.stderr
        preserved = output.read_bytes()
        if sys.platform == "darwin":
            deployment = environment | {"MACOSX_DEPLOYMENT_TARGET": "11.0"}
            rejected_deployment = run(
                [gti, "build", "app", "--backend", "llvm"], project,
                expected=78, env=deployment)
            assert "matching target-built runtime" in rejected_deployment.stderr
            assert output.read_bytes() == preserved
        dependency_manifest = dependency / "gti.toml"
        old_dependency_manifest = dependency_manifest.read_text()
        dependency_manifest.write_text(
            'manifest-version = 1\n[package]\nname = "cache-dependency"\n'
            'version = "1.0.0"\n[package.build-script]\n'
            'source = "build.gti"\ninputs = []\n', encoding="utf-8")
        (dependency / "build.gti").write_text(
            '#include <std/build>\n#include <std/string>\n#include <std/vector>\n'
            'int main(int argc, std::vector<std::string> argv) {\n'
            '  mut auto context = std::build::context_from_args(argv);\n'
            '  mut auto outputs = std::build::outputs();\n'
            '  return outputs.publish(context);\n}\n', encoding="utf-8")
        unsupported_script = invoke("build", "app", "--backend", "llvm", expected=65)
        assert "GTI-B0003" in unsupported_script.stderr, unsupported_script.stderr
        assert output.read_bytes() == preserved
        dependency_manifest.write_text(old_dependency_manifest, encoding="utf-8")
        (project / "src/main.gti").write_text(
            'extern "C" { int32_t cache_native_value(); }\n'
            "int main() { return cache_native_value(); }\n", encoding="utf-8")
        foreign = invoke("build", "app", "--cxx", wrapper, expected=65)
        assert "GTI-B0003" in foreign.stderr and "MirDefinitionKind.Declaration" in foreign.stderr
        assert output.read_bytes() == preserved
        (project / "src/main.gti").write_text(
            "class Dynamic { public: virtual int value() { return 1; } }; "
            "int main() { Dynamic value{}; return value.value(); }\n",
            encoding="utf-8")
        rejected = invoke("build", "app", "--cxx", wrapper, expected=65)
        assert "GTI-B0003" in rejected.stderr, rejected.stderr
        assert output.read_bytes() == preserved
        manifest_path.write_text(manifest.replace('backend = "llvm"', 'backend = "bad"'),
                                 encoding="utf-8")
        invalid = invoke("metadata", expected=65)
        assert "GTI-B1005" in invalid.stderr and "backend" in invalid.stderr
        backend_line = manifest.count("\n", 0, manifest.index('backend = "llvm"')) + 1
        assert f"gti.toml:{backend_line}:11:" in invalid.stderr, invalid.stderr
    print("LLVM project/native dependency/cache/run/install/backend isolation passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
