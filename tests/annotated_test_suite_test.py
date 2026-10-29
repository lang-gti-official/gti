#!/usr/bin/env python3
"""Verify semantic registration, isolated execution, reporting and cache reuse."""

import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile


def main():
    compiler = Path(sys.argv[1]).resolve()
    root = Path(__file__).resolve().parents[1]
    environment = dict(os.environ, GTI_STDLIB_PATH=str(root / "stdlib"), NO_COLOR="1")
    for variable in ("GTI_CXX", "CXX", "CPATH", "CPLUS_INCLUDE_PATH", "LIBRARY_PATH"):
        environment.pop(variable, None)

    def run(directory, *arguments, expected=0):
        result = subprocess.run([str(compiler), *arguments], cwd=directory,
                                env=environment, capture_output=True, text=True, timeout=180)
        output = result.stdout + result.stderr
        assert result.returncode == expected, (arguments, result.returncode, output)
        return output

    with tempfile.TemporaryDirectory(prefix="gti-annotated-tests-") as temporary:
        project = Path(temporary)
        shutil.copytree(root / "examples/annotated-tests", project, dirs_exist_ok=True)
        for standard, level in (("c++20", "-O0"), ("c++23", "-O3")):
            output = run(project, "test", "--std", standard, level, "--no-cache")
            assert "Test result: 3 passed" in output, output
            for name in ("another_fresh_scene_state", "event_value_is_preserved", "fresh_scene_state"):
                assert f"engine::tests::{name} ... passed" in output, output

        first = run(project, "test", "--verbose")
        second = run(project, "test", "--verbose")
        assert "Test result: 3 passed" in first and "Test result: 3 passed" in second
        assert "cache hit" in second.lower(), second
        metadata = next((project / "build/gti/cache").rglob("metadata"))
        poisoned = metadata.read_text().replace("case engine::tests::fresh_scene_state", "case engine::tests::wrong_name")
        metadata.write_text(poisoned)
        recovered = run(project, "test", "--verbose")
        assert "Test result: 3 passed" in recovered and "wrong_name ..." not in recovered, recovered
        assert "case engine::tests::fresh_scene_state" in metadata.read_text()

        # Debug symbols and the test catalog coexist in the same cache entry.
        debug_first = run(project, "test", "-g", "--verbose")
        debug_second = run(project, "test", "-g", "--verbose")
        assert "Test result: 3 passed" in debug_first and "Test result: 3 passed" in debug_second
        assert "cache hit" in debug_second.lower(), debug_second
        if sys.platform == "darwin":
            executable = next((project / "build/gti/dev").glob("*/engine-tests"))
            symbols = Path(str(executable) + ".dSYM")
            assert symbols.is_dir(), debug_second
            shutil.rmtree(symbols)
            executable.unlink()
            restored = run(project, "test", "-g", "--verbose")
            assert "cache hit" in restored.lower() and "Test result: 3 passed" in restored
            assert executable.is_file() and symbols.is_dir(), restored

        source = project / "tests/main.gti"
        source.write_text('''import std.testing;
import std.string;
extern "C" { int raise(int signal); }
namespace sample {
  struct Guard {
    ~Guard() { std::println("guard-cleaned"); }
   public: Guard() = default; };
  [[test]] void a_success() { std::testing::check(true); }
  [[test]] void b_panic() { Guard guard{}; std::testing::check(false, "isolated panic"); }
  [[test]] void c_signal() { unsafe { [[discard]] raise(9); } }
  [[test]] void d_after_panic() { std::println("after-panic-ran"); }
}
''')
        output = run(project, "test", "--no-cache", expected=70)
        assert "sample::a_success ... passed" in output, output
        assert "sample::b_panic ... failed" in output, output
        assert "isolated panic" in output and "guard-cleaned" in output, output
        assert "sample::c_signal ... crashed" in output and "exit code 137" in output, output
        assert "sample::d_after_panic ... passed" in output and "after-panic-ran" in output, output
        assert "Test result: 2 passed, 2 failed" in output, output
        assert "main.gti:" in output, output

        # Discovery traverses the suite graph without granting the user's test
        # bodies access to private declarations in other source units.
        (project / "tests/leaf.gti").write_text(
            (
                'export module leaf;\n'
                'namespace leaf { export [[test]] void nested() {} }\n'
            ))
        (project / "tests/indirect.gti").write_text((
                                                        'export module indirect;\n'
                                                        'import "leaf.gti";\n'
                                                    ))
        source.write_text('import "indirect.gti";\n[[test]] void root_case() {}\n')
        output = run(project, "test", "--no-cache")
        assert "leaf::nested ... passed" in output and "Test result: 2 passed" in output, output
        source.write_text('import "indirect.gti";\n[[test]] void root_case() { leaf::nested(); }\n')
        output = run(project, "check", "engine-tests", expected=65)
        assert "GTI-S2024" in output, output

        # Imported module tests remain discoverable even when the declaration
        # itself is private; dependency package tests are excluded.
        manifest = project / "gti.toml"
        manifest.write_text(manifest.read_text() + '\n[modules]\ncases = "tests/module.gti"\n')
        (project / "tests/module.gti").write_text(
            "export module cases; namespace module_cases { [[test]] void hidden() {} }\n")
        source.write_text("import cases;\n[[test]] void root_case() {}\n")
        output = run(project, "test", "--no-cache")
        assert "module_cases::hidden ... passed" in output and "Test result: 2 passed" in output, output

        helper = project / "helper"
        (helper / "src").mkdir(parents=True)
        (helper / "gti.toml").write_text((
                                             'manifest-version = 1\n'
                                             '[package]\n'
                                             'name = "helper"\n'
                                             'version = "0.1.0"\n'
                                             '\n'
                                             '[modules]\n'
                                             'cases = "src/cases.gti"\n'
                                         ))
        (helper / "src/cases.gti").write_text(
            (
                'export module cases;\n'
                'import std.testing;\n'
                'namespace dependency_cases { export [[test]] void never_run() { std::testing::check(false); } }\n'
            ))
        manifest.write_text(manifest.read_text() + '\n[dependencies]\nhelper = { path = "helper" }\n')
        source.write_text('import helper.cases;\n[[test]] void root_case() {}\n')
        output = run(project, "test", "--no-cache")
        assert "Test result: 1 passed" in output and "never_run ..." not in output, output

        # Dispatcher locals are hygienic even when test functions use its
        # preferred scratch names, including successive fallback names.
        source.write_text("\n".join(
            f"[[test]] void {name}() {{}}"
            for name in ("gti_test_argc", "gti_test_argc_", "gti_test_argv", "gti_test_argv_")) + "\n")
        output = run(project, "test", "--no-cache")
        assert "Test result: 4 passed" in output, output

        invalid = [
            "[[test]] int invalid() { return 0; }",
            "[[test]] void invalid(int parameter) {}",
            "[[test]] void invalid<T>() {}",
            "[[test]] void invalid();",
            "[[test, test]] void invalid() {}",
            "struct Object { [[test]] void invalid() {}  public: Object() = default; };",
            "[[test(value)]] void invalid() {}",
            "[[test]] static void invalid() {}",
        ]
        for text in invalid:
            source.write_text(text)
            output = run(project, "check", "engine-tests", expected=65)
            code = "GTI-P0001" if text.startswith("[[test(value)") else "GTI-S2099"
            assert code in output and ("test" in output or "attribute" in output), (text, output)

        source.write_text("[[test]] void case_one() {}\nint main() { return 0; }\n")
        output = run(project, "test", "--no-cache", expected=65)
        assert "entry point" in output and "main" in output, output
        source.write_text("")
        output = run(project, "test", "--no-cache", expected=65)
        assert "GTI-S2094" in output and "annotated test" in output, output

        source.write_text("int main() { std::println(\"legacy-ran\"); return 0; }\n")
        output = run(project, "test", "--no-cache")
        assert output.count("legacy-ran") == 1 and "Test result: 1 passed" in output, output

    print("annotated test suites passed: registration, reporting, isolation, cleanup, cache and legacy main")


if __name__ == "__main__":
    main()
