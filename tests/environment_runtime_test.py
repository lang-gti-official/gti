#!/usr/bin/env python3
"""Owned environment lookups, module API and example across native modes."""

import os
import pathlib
import subprocess
import sys
import tempfile

from native_symbol_spelling import native_spelling


def run(command, environment, status=0, output=None):
    result = subprocess.run([str(item) for item in command], env=environment,
                            capture_output=True, text=True, timeout=180)
    assert result.returncode == status, (command, result.stdout, result.stderr)
    if output is not None:
        assert (result.stdout, result.stderr) == (output, ""), (
            command, result.stdout, result.stderr)
    return result


def main():
    compiler = pathlib.Path(sys.argv[1]).resolve()
    library = pathlib.Path(sys.argv[2]).resolve()
    root = pathlib.Path(__file__).resolve().parents[1]
    environment = dict(os.environ, GTI_STDLIB_PATH=str(root / "stdlib"),
                       GTI_RUNTIME_INCLUDE=str(root / "runtime/include"),
                       GTI_RUNTIME_LIBRARY=str(library),
                       GTI_VENDOR_INCLUDE=str(root / "vendor/expected_lite/include"),
                       GTI_ENVIRONMENT_EMPTY="",
                       GTI_ENVIRONMENT_SPACES="two words = exact value",
                       GTI_ENVIRONMENT_UNICODE="café",
                       GTI_ENVIRONMENT_LARGE="x" * 16384,
                       ASAN_OPTIONS="halt_on_error=1",
                       UBSAN_OPTIONS="halt_on_error=1")
    environment.pop("GTI_ENVIRONMENT_MISSING_7d4c9f3e", None)
    environment.pop("GTI_DEMO_PROFILE", None)
    with tempfile.TemporaryDirectory(prefix="gti-environment-") as temporary:
        scratch = pathlib.Path(temporary)
        module = scratch / "module.gti"
        module.write_text('import std.environment;\nint main() {\n'
                          'auto value = std::environment::get("A=B");\n'
                          'if (value || value.error() != std::environment::errc::invalid_name) { return 1; }\n'
                          'return 0; }\n', encoding="utf-8")
        cases = [(root / "tests/fixtures/stdlib_surface_environment.gti", "surface"),
                 (root / "tests/fixtures/environment_lookup.gti", "lookup"),
                 (root / "examples/138-environment-configuration.gti", "example"),
                 (module, "module")]
        for standard in ("c++20", "c++23"):
            for level in ("O0", "O3"):
                for source, name in cases:
                    binary = scratch / f"{name}-{standard}-{level}"
                    run([compiler, source, "--std", standard, f"-{level}",
                         "--keep-cpp", "-o", binary], environment)
                    if name == "example":
                        run([binary], environment, output="profile: default\n")
                        run([binary], dict(environment, GTI_DEMO_PROFILE=""),
                            output="profile: explicitly empty\n")
                        run([binary], dict(environment, GTI_DEMO_PROFILE="development"),
                            output="development\n")
                    else:
                        run([binary], environment, output="")
                    generated = binary.with_suffix(".gti.cpp").read_text()
                    assert "// GTI verified-MIR body:" in generated
                    assert native_spelling(generated, "gti_rt_environment_get_v1") in generated
                    assert native_spelling(generated, "gti_rt_environment_snapshot_destroy_v1") in generated
        # Instrumented generated code exercises the temporary snapshot owner
        # and owned string conversion. The native boundary is independently
        # instrumented by the runtime test / CI sanitizer configurations.
        binary = scratch / "sanitized"
        run([compiler, cases[1][0], "-O1", "-o", binary, "--",
             "-fsanitize=address,undefined", "-fno-omit-frame-pointer"], environment)
        run([binary], environment, output="")

        windows = run([compiler, module, "--target", "x86_64-pc-windows-msvc",
                       "--emit-cpp", "-o", scratch / "windows.cpp"], environment, status=65)
        assert "std/environment is not yet available on Windows" in windows.stderr
        dangling = scratch / "dangling.gti"
        dangling.write_text('#include <std/environment>\n#include <std/string>\n'
                            'std::string_view bad() {\n'
                            'auto lookup = std::environment::get("PATH");\n'
                            'auto value = std::move(lookup).expect("environment lookup");\n'
                            'return value.value().view(); }\n'
                            'int main() { return 0; }\n', encoding="utf-8")
        rejected = run([compiler, dangling, "--emit-cpp", "-o", scratch / "bad.cpp"],
                       environment, status=65)
        assert "borrow" in rejected.stderr.lower(), rejected.stderr
    print("owned environment lookup, module, example, target and lifetime contracts passed")


if __name__ == "__main__":
    main()
