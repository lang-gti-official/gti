#!/usr/bin/env python3

import json
import os
import pathlib
import shutil
import subprocess
import sys
import tempfile


def run(arguments, expected=0, cwd=None, env=None):
    result = subprocess.run(
        arguments, cwd=cwd, env=env, text=True, capture_output=True, check=False
    )
    if result.returncode != expected:
        raise AssertionError(
            f"command returned {result.returncode}, expected {expected}\n"
            f"command: {arguments}\nstdout: {result.stdout}\nstderr: {result.stderr}"
        )
    return result


def executable_named(directory, name):
    filename = f"{name}.exe" if sys.platform == "win32" else name
    matches = list(directory.glob(f"*/{filename}"))
    if len(matches) != 1:
        raise AssertionError(f"expected one {filename} below {directory}: {matches}")
    return matches[0]


def manifest(targets, profiles=""):
    return (
        "manifest-version = 1\n\n"
        "[package]\n"
        'name = "sample"\n'
        'version = "0.1.0"\n\n'
        f"{targets}\n"
        f"{profiles}"
    )


def host_target_os():
    if sys.platform == "darwin":
        return "macos"
    if sys.platform.startswith("linux"):
        return "linux"
    if sys.platform == "win32":
        return "windows"
    return "unknown"


def main():
    if len(sys.argv) != 2:
        raise SystemExit("usage: project_cli_smoke_test.py /path/to/gti")

    requested_gti = pathlib.Path(sys.argv[1])
    resolved_gti = (
        requested_gti.resolve()
        if requested_gti.exists()
        else shutil.which(sys.argv[1])
    )
    if resolved_gti is None:
        raise AssertionError(f"could not resolve GTI executable: {sys.argv[1]}")
    gti = str(resolved_gti)
    scaffold_source = (
        "#include <std/string>\n"
        "#include <std/vector>\n"
        "\n"
        "int main(int argc, std::vector<std::string> argv) {\n"
        '  std::println("Hello, GTI!");\n'
        "  return 0;\n"
        "}\n"
    )
    with tempfile.TemporaryDirectory(prefix="gti-project-cli-test-") as directory:
        root = pathlib.Path(directory)

        new_project = root / "new-project"
        created = run([gti, "new", str(new_project)])
        assert "Created package 'new-project'" in created.stdout
        assert (new_project / "gti.toml").is_file()
        assert (new_project / "src/main.gti").is_file()
        assert (
            new_project.joinpath("src/main.gti").read_text(encoding="utf-8")
            == scaffold_source
        )
        new_metadata = json.loads(run([gti, "metadata"], cwd=new_project).stdout)
        assert new_metadata["package"]["name"] == "new-project"
        run([gti, "check"], cwd=new_project)
        generated_run = run([gti, "run"], cwd=new_project)
        assert generated_run.stdout == "Hello, GTI!\n"
        existing_new = run(
            [gti, "new", str(new_project)], expected=64, cwd=root
        )
        assert "error[GTI-B1501]" in existing_new.stderr

        invalid_project = root / "123-invalid"
        invalid_name = run(
            [gti, "new", str(invalid_project)], expected=64, cwd=root
        )
        assert "error[GTI-B1500]" in invalid_name.stderr
        assert not invalid_project.exists()
        run(
            [gti, "new", str(invalid_project), "--name", "valid_name"],
            cwd=root,
        )
        override_metadata = json.loads(
            run([gti, "metadata"], cwd=invalid_project).stdout
        )
        assert override_metadata["package"]["name"] == "valid_name"

        reserved_project = root / "reserved-project"
        reserved_name = run(
            [gti, "new", str(reserved_project), "--name", "CON"],
            expected=64,
            cwd=root,
        )
        assert "reserved portable device name" in reserved_name.stderr
        assert not reserved_project.exists()

        init_project = root / "init-project"
        init_source = init_project / "src/main.gti"
        init_source.parent.mkdir(parents=True)
        init_contents = "int main() { return 0; }\n"
        init_source.write_text(init_contents, encoding="utf-8")
        initialized = run([gti, "init", str(init_project)])
        assert "Initialized package 'init-project'" in initialized.stdout
        assert init_source.read_text(encoding="utf-8") == init_contents
        assert (init_project / "gti.toml").is_file()
        repeated_init = run(
            [gti, "init", str(init_project)], expected=64, cwd=root
        )
        assert "error[GTI-B1503]" in repeated_init.stderr

        default_init = root / "default-init"
        default_init.mkdir()
        run([gti, "init"], cwd=default_init)
        assert (default_init / "gti.toml").is_file()
        assert (default_init / "src/main.gti").is_file()
        assert (
            default_init.joinpath("src/main.gti").read_text(encoding="utf-8")
            == scaffold_source
        )

        format_root = root / "format-root"
        format_root.mkdir()
        initialized_format = run([gti, "format", "init"], cwd=format_root)
        format_config = format_root / ".gti-format"
        assert "Created format configuration" in initialized_format.stdout
        assert format_config.is_file()
        format_contents = format_config.read_text(encoding="utf-8")
        assert format_contents.startswith("BasedOnStyle: GTI\n")
        assert "ReferenceAlignment: Left\n" in format_contents

        explicit_format_root = root / "explicit-format-root"
        explicit_format_root.mkdir()
        run([gti, "format", "init", str(explicit_format_root)], cwd=root)
        assert (explicit_format_root / ".gti-format").read_text(
            encoding="utf-8"
        ) == format_contents

        repeated_format_init = run(
            [gti, "format", "init"], expected=64, cwd=format_root
        )
        assert "error[GTI-B1503]" in repeated_format_init.stderr
        assert format_config.read_text(encoding="utf-8") == format_contents

        missing_format_destination = run(
            [gti, "format", "init", str(root / "missing-format-root")],
            expected=64,
            cwd=root,
        )
        assert "error[GTI-B1502]" in missing_format_destination.stderr

        missing_format_subcommand = run(
            [gti, "format"], expected=64, cwd=root
        )
        assert "requires the 'init' subcommand" in missing_format_subcommand.stderr
        unknown_format_option = run(
            [gti, "format", "init", "--unknown"], expected=64, cwd=root
        )
        assert "unknown format init option" in unknown_format_option.stderr

        intermediate_project = root / "intermediate-target"
        intermediate_source = intermediate_project / "src/main.gti"
        intermediate_source.parent.mkdir(parents=True)
        intermediate_source.write_text(
            "int main() { return 0; }\n", encoding="utf-8"
        )
        (intermediate_project / "gti.toml").write_text(
            "manifest-version = 1\n\n"
            "[package]\n"
            'name = "intermediate_target"\n'
            'version = "0.1.0"\n\n'
            "[targets.intermediate]\n"
            'kind = "executable"\n'
            'root = "src/main.gti"\n\n'
            "[profiles.dev]\n"
            "keep-cpp = true\n",
            encoding="utf-8",
        )
        run([gti, "build"], cwd=intermediate_project)
        intermediate_executable = executable_named(
            intermediate_project / "build/gti/dev", "intermediate"
        )
        run([str(intermediate_executable)])
        assert (
            intermediate_executable.parent
            / ".gti-intermediate/intermediate.gti.cpp"
        ).is_file()

        missing_new_path = run([gti, "new"], expected=64, cwd=root)
        assert "requires a destination path" in missing_new_path.stderr
        unknown_init_option = run(
            [gti, "init", "--unknown"], expected=64, cwd=root
        )
        assert "unknown init option" in unknown_init_option.stderr

        protected_emit_source = root / "protected-emit.gti"
        protected_emit_contents = "int main() { return 0; }\n"
        protected_emit_source.write_text(protected_emit_contents, encoding="utf-8")
        protected_emit = run(
            [
                gti,
                str(protected_emit_source),
                "--emit-cpp",
                "-o",
                str(protected_emit_source),
            ],
            expected=64,
        )
        assert "refusing to overwrite loaded source" in protected_emit.stderr
        assert (
            protected_emit_source.read_text(encoding="utf-8")
            == protected_emit_contents
        )

        protected_binary_source = root / "protected-binary.gti"
        protected_binary_contents = "int main() { return 0; }\n"
        protected_binary_source.write_text(
            protected_binary_contents, encoding="utf-8"
        )
        protected_binary = run(
            [gti, str(protected_binary_source), "-o", str(protected_binary_source)],
            expected=64,
        )
        assert "refusing to overwrite loaded source" in protected_binary.stderr
        assert (
            protected_binary_source.read_text(encoding="utf-8")
            == protected_binary_contents
        )

        unsafe_output_project = root / "unsafe-output-project"
        unsafe_output_project.mkdir()
        (unsafe_output_project / "main.gti").write_text(
            "int main() { return 0; }\n", encoding="utf-8"
        )
        (unsafe_output_project / "gti.toml").write_text(
            manifest(
                "[targets.sample]\n"
                'kind = "executable"\n'
                'root = "main.gti"\n'
            ),
            encoding="utf-8",
        )
        external_build = root / "external-build"
        external_build.mkdir()
        try:
            (unsafe_output_project / "build").symlink_to(
                external_build, target_is_directory=True
            )
        except OSError:
            pass
        else:
            unsafe_output = run(
                [gti, "build"], expected=74, cwd=unsafe_output_project
            )
            assert "symbolic-link" in unsafe_output.stderr
            assert not (external_build / "gti").exists()

        project = root / "project"
        source = project / "src/main.gti"
        nested = project / "src/nested"
        source.parent.mkdir(parents=True)
        nested.mkdir(parents=True)
        source.write_text("int main() { return 0; }\n", encoding="utf-8")
        targets = (
            "[targets.sample]\n"
            'kind = "executable"\n'
            'root = "src/main.gti"\n'
        )
        profiles = (
            "\n[profiles.dev]\n"
            "keep-cpp = true\n"
            "\n[profiles.release]\n"
            "optimization = 3\n"
            'cpp-standard = "c++20"\n'
            'execution-profile = "concurrent"\n'
            "keep-cpp = false\n"
        )
        manifest_path = project / "gti.toml"
        manifest_path.write_text(manifest(targets, profiles), encoding="utf-8")

        built = run([gti, "build"], cwd=nested)
        assert "Built sample [dev," in built.stdout
        dev_directory = project / "build/gti/dev"
        executable = executable_named(dev_directory, "sample")
        run([str(executable)])
        assert not (project / "build/gti/release").exists()

        emitted = run([gti, "build", "--emit-mir"], cwd=nested)
        assert "Emitted MIR sample [dev," in emitted.stdout
        mir_artifacts = sorted(dev_directory.rglob("sample.mir"))
        assert len(mir_artifacts) == 1
        mir_head = mir_artifacts[0].read_text(encoding="utf-8").splitlines()[0]
        assert mir_head.startswith("mir-v") and "valid=1" in mir_head
        conflicted = run(
            [gti, "build", "--emit-mir", "--keep-cpp"],
            expected=64,
            cwd=nested,
        )
        assert (
            "--emit-mir and --keep-cpp cannot be used together"
            in conflicted.stderr
        )
        unsupported = run(
            [gti, "run", "--emit-mir"], expected=64, cwd=nested
        )
        assert "--emit-mir is not supported by gti run" in unsupported.stderr

        generated = executable.parent / ".gti-intermediate/sample.gti.cpp"
        assert generated.is_file()
        direct_cpp = root / "direct.cpp"
        run([gti, str(source), "--emit-cpp", "-o", str(direct_cpp)])
        assert generated.read_bytes() == direct_cpp.read_bytes()

        cached_build = run([gti, "build", "--verbose"], cwd=project)
        assert "gti: cache hit " in cached_build.stderr
        assert not any(
            line.startswith("+ ") for line in cached_build.stderr.splitlines()
        )
        executable.unlink()
        restored_build = run([gti, "build", "--verbose"], cwd=project)
        assert "gti: cache hit " in restored_build.stderr
        assert executable.is_file()
        run([str(executable)])

        uncached_build = run(
            [gti, "build", "--no-cache", "--verbose"], cwd=project
        )
        assert "cache disabled by --no-cache" in uncached_build.stderr
        assert any(
            line.startswith("+ ") for line in uncached_build.stderr.splitlines()
        )

        cache_executables = list((project / "build/gti/cache/v2").glob(
            "*/executable"
        ))
        assert len(cache_executables) == 1
        cache_executables[0].write_bytes(b"corrupt")
        recovered_cache = run([gti, "build", "--verbose"], cwd=project)
        assert "ignored corrupt build cache entry" in recovered_cache.stderr
        assert "gti: cache recovered " in recovered_cache.stderr
        assert any(
            line.startswith("+ ") for line in recovered_cache.stderr.splitlines()
        )

        run([gti, "build", "--no-keep-cpp"], cwd=project)
        assert not generated.exists()

        release = run(
            [gti, "build", "sample", "--release", "--verbose"], cwd=project
        )
        assert " -O3 " in release.stderr
        assert " -std=c++20 " in release.stderr
        assert "execution-profile=concurrent" in release.stderr
        assert "target sample [release," in release.stderr
        assert "Built sample [release," in release.stdout
        release_executable = executable_named(project / "build/gti/release", "sample")
        run([str(release_executable)])
        release_generated = (
            release_executable.parent / ".gti-intermediate/sample.gti.cpp"
        )
        assert not release_generated.exists()

        overridden = run(
            [gti, "build", "--profile", "release", "-O1", "--keep-cpp", "--verbose"],
            cwd=project,
        )
        assert " -O1 " in overridden.stderr
        assert release_generated.is_file()

        conflict = run(
            [gti, "build", "--release", "--profile", "release"],
            expected=64,
            cwd=project,
        )
        assert "cannot be combined" in conflict.stderr

        profile_as_target = run(
            [gti, "build", "release"], expected=65, cwd=project
        )
        assert "is a profile" in profile_as_target.stderr
        assert "--profile release" in profile_as_target.stderr

        check_project = root / "check-project"
        check_project.mkdir()
        (check_project / "main.gti").write_text(
            "int main() { return 0; }\n", encoding="utf-8"
        )
        (check_project / "gti.toml").write_text(
            manifest(
                "[targets.sample]\n"
                'kind = "executable"\n'
                'root = "main.gti"\n',
                "\n[profiles.release]\n"
                "optimization = 3\n"
                'execution-profile = "concurrent"\n',
            ),
            encoding="utf-8",
        )
        metadata = run([gti, "metadata"], cwd=check_project)
        metadata_document = json.loads(metadata.stdout)
        assert metadata_document["schemaVersion"] == 8
        assert metadata_document["manifestVersion"] == 1
        assert metadata_document["package"]["name"] == "sample"
        assert metadata_document["profiles"] == [
            {
                "name": "dev",
                "optimization": 0,
                "cppStandard": "c++23",
                "executionProfile": "single-threaded",
                "keepCpp": False,
            },
            {
                "name": "release",
                "optimization": 3,
                "cppStandard": "c++23",
                "executionProfile": "concurrent",
                "keepCpp": False,
            },
        ]
        assert metadata_document["targets"][0]["outputs"][1]["profile"] == "release"
        assert metadata_document["targets"][0]["outputs"][0]["native"] == {
            "includeDirectories": [],
            "cSources": [],
            "cStandard": "c17",
            "cCompileArguments": [],
            "cppSources": [],
            "compileArguments": [],
            "libraryDirectories": [],
            "linkFiles": [],
            "libraries": [],
            "frameworks": [],
            "orderedLinkOperands": [],
            "linkArguments": [],
            "rawArguments": [],
            "dependencyNative": [],
        }
        assert not (check_project / "build").exists()

        no_test_targets = run(
            [gti, "test"], expected=65, cwd=check_project
        )
        assert "error[GTI-B1203]" in no_test_targets.stderr
        assert "kind = \"test\"" in no_test_targets.stderr

        test_project = root / "test-project"
        (test_project / "src").mkdir(parents=True)
        (test_project / "tests").mkdir()
        (test_project / "src/main.gti").write_text(
            "int main() { return 0; }\n", encoding="utf-8"
        )
        (test_project / "tests/alpha.gti").write_text(
            "int main() { return 7; }\n", encoding="utf-8"
        )
        (test_project / "tests/beta.gti").write_text(
            "int main() { return 0; }\n", encoding="utf-8"
        )
        (test_project / "gti.toml").write_text(
            manifest(
                "[targets.beta]\n"
                'kind = "test"\n'
                'root = "tests/beta.gti"\n\n'
                "[targets.app]\n"
                'kind = "executable"\n'
                'root = "src/main.gti"\n\n'
                "[targets.alpha]\n"
                'kind = "test"\n'
                'root = "tests/alpha.gti"\n'
            ),
            encoding="utf-8",
        )
        test_metadata = json.loads(
            run([gti, "metadata"], cwd=test_project).stdout
        )
        assert test_metadata["schemaVersion"] == 8
        assert [
            (target["name"], target["kind"])
            for target in test_metadata["targets"]
        ] == [
            ("alpha", "test"),
            ("app", "executable"),
            ("beta", "test"),
        ]

        default_mixed_build = run([gti, "build"], cwd=test_project)
        assert "Built app [dev," in default_mixed_build.stdout
        default_mixed_run = run([gti, "run"], cwd=test_project)
        assert "Built app [dev," in default_mixed_run.stderr

        all_tests = run([gti, "test"], expected=7, cwd=test_project)
        assert all_tests.stderr.index("Building test alpha") < all_tests.stderr.index(
            "Building test beta"
        )
        assert all_tests.stderr.index("Testing alpha") < all_tests.stderr.index(
            "Testing beta"
        )
        assert "Failed alpha (exit code 7)" in all_tests.stderr
        assert "Passed beta" in all_tests.stderr
        assert "Test result: 1 passed, 1 failed" in all_tests.stderr

        selected_test = run(
            [gti, "test", "beta", "--release", "--verbose"],
            cwd=test_project,
        )
        assert "Testing beta" in selected_test.stderr
        assert "Testing alpha" not in selected_test.stderr
        assert "target beta [release," in selected_test.stderr
        assert "Test result: 1 passed" in selected_test.stderr

        wrong_test_kind = run(
            [gti, "test", "app"], expected=65, cwd=test_project
        )
        assert "error[GTI-B1204]" in wrong_test_kind.stderr
        assert "not a test target" in wrong_test_kind.stderr
        unknown_test = run(
            [gti, "test", "btea"], expected=65, cwd=test_project
        )
        assert "error[GTI-B1200]" in unknown_test.stderr
        assert "Did you mean 'beta'?" in unknown_test.stderr
        wrong_run_kind = run(
            [gti, "run", "alpha"], expected=64, cwd=test_project
        )
        assert "use gti test alpha" in wrong_run_kind.stderr
        run([gti, "test", "--"], expected=64, cwd=test_project)

        unusable_environment = os.environ.copy()
        unusable_environment["CXX"] = "definitely-not-a-native-compiler"
        checked = run(
            [gti, "check", "--release", "--verbose"],
            cwd=check_project,
            env=unusable_environment,
        )
        assert "Checked sample [release," in checked.stdout
        assert "source" in checked.stderr
        assert not (check_project / "build").exists()

        invalid_check_option = run(
            [gti, "check", "--cxx", "unused"], expected=64, cwd=check_project
        )
        assert "not valid for gti check" in invalid_check_option.stderr
        invalid_c_check_option = run(
            [gti, "check", "--cc", "unused"], expected=64, cwd=check_project
        )
        assert "not valid for gti check" in invalid_c_check_option.stderr
        invalid_cache_check_option = run(
            [gti, "check", "--no-cache"], expected=64, cwd=check_project
        )
        assert "--no-cache is not valid for gti check" in (
            invalid_cache_check_option.stderr
        )

        movable_project = root / "movable-project"
        run([gti, "new", str(movable_project)])
        first_movable_build = run(
            [gti, "build", "--verbose"], cwd=movable_project
        )
        assert "gti: cache miss " in first_movable_build.stderr
        moved_project = root / "moved-project"
        movable_project.rename(moved_project)
        moved_build = run([gti, "build", "--verbose"], cwd=moved_project)
        assert "gti: cache hit " in moved_build.stderr
        assert not any(
            line.startswith("+ ") for line in moved_build.stderr.splitlines()
        )

        policy_project = root / "concurrent-policy-project"
        policy_project.mkdir()
        (policy_project / "main.gti").write_text(
            "mut int state = 0;\nint main() { return state; }\n",
            encoding="utf-8",
        )
        (policy_project / "gti.toml").write_text(
            manifest(
                "[targets.sample]\n"
                'kind = "executable"\n'
                'root = "main.gti"\n',
                "\n[profiles.release]\n"
                'execution-profile = "concurrent"\n',
            ),
            encoding="utf-8",
        )
        run([gti, "check"], cwd=policy_project)
        profile_rejection = run(
            [gti, "check", "--release"], expected=65, cwd=policy_project
        )
        assert "error[GTI-S2060]" in profile_rejection.stderr
        assert "requires namespace global 'state' to be immutable" in (
            profile_rejection.stderr
        )
        run(
            [
                gti,
                "check",
                "--release",
                "--execution-profile",
                "single-threaded",
            ],
            cwd=policy_project,
        )

        native_project = root / "native-project"
        native_source = native_project / "src/main.gti"
        native_nested = native_project / "src/nested"
        native_directory = native_project / "native"
        native_nested.mkdir(parents=True)
        native_directory.mkdir(parents=True)
        native_source.write_text(
            'namespace native {\nextern "C" {\n'
            "  int32_t gti_project_native_add(int32_t left, int32_t right);\n"
            "  int32_t gti_project_native_product(int32_t left, int32_t right);\n"
            "}\n}\n"
            "int main() {\n"
            "  if (native::gti_project_native_add(20, 22) == 42 and\n"
            "      native::gti_project_native_product(6, 7) == 42) { return 0; }\n"
            "  return 1;\n"
            "}\n",
            encoding="utf-8",
        )
        native_implementation = native_directory / "native abi.c"
        native_c_contents = (
            "#include <gti/c_abi.h>\n"
            "#if !defined(GTI_PROJECT_C_BASE) || "
            "!defined(GTI_PROJECT_C_PLATFORM)\n"
            '#error "missing resolved native C arguments"\n'
            "#endif\n"
            "int32_t gti_project_native_add(\n"
            "    int32_t left, int32_t right) {\n"
            "  return left + right;\n"
            "}\n"
        )
        native_implementation.write_text(native_c_contents, encoding="utf-8")
        native_cpp_implementation = native_directory / "native support.cpp"
        native_cpp_contents = (
            "#include <gti/c_abi.h>\n"
            "#if !defined(GTI_PROJECT_NATIVE_BASE) || "
            "!defined(GTI_PROJECT_NATIVE_PLATFORM)\n"
            '#error "missing resolved native C++ arguments"\n'
            "#endif\n"
            'extern "C" int32_t gti_project_native_product(\n'
            "    int32_t left, int32_t right) {\n"
            "  return left * right;\n"
            "}\n"
        )
        native_cpp_implementation.write_text(
            native_cpp_contents, encoding="utf-8"
        )
        native_c_compiler = shutil.which("cc")
        if native_c_compiler is None:
            raise AssertionError("project native-source test requires cc on PATH")
        native_cpp_compiler = shutil.which("c++")
        if native_cpp_compiler is None:
            raise AssertionError("project native-source test requires c++ on PATH")

        selected_os = host_target_os()
        unselected_os = "macos" if selected_os != "macos" else "linux"
        (native_project / "gti.toml").write_text(
            "manifest-version = 1\n\n"
            "[package]\n"
            'name = "native-project"\n'
            'version = "0.1.0"\n\n'
            "[package.native]\n"
            'c-sources = ["native/native abi.c"]\n'
            'cpp-sources = ["native/native support.cpp"]\n'
            'c-standard = "c17"\n'
            'c-compile-args = ["-DGTI_PROJECT_C_BASE=1"]\n'
            'compile-args = ["-DGTI_PROJECT_NATIVE_BASE=1"]\n\n'
            "[[package.native.platforms]]\n"
            f'os = "{selected_os}"\n'
            'c-compile-args = ["-DGTI_PROJECT_C_PLATFORM=1"]\n'
            'compile-args = ["-DGTI_PROJECT_NATIVE_PLATFORM=1"]\n'
            "\n"
            "[[package.native.platforms]]\n"
            f'os = "{unselected_os}"\n'
            'raw-args = ["--gti-unselected-native-platform"]\n\n'
            "[targets.native-project]\n"
            'kind = "executable"\n'
            'root = "src/main.gti"\n',
            encoding="utf-8",
        )

        native_metadata = run([gti, "metadata"], cwd=native_nested)
        repeated_native_metadata = run([gti, "metadata"], cwd=native_nested)
        assert repeated_native_metadata.stdout == native_metadata.stdout
        native_document = json.loads(native_metadata.stdout)
        native_inputs = native_document["targets"][0]["outputs"][0]["native"]
        assert native_document["schemaVersion"] == 8
        assert native_inputs["cSources"] == [str(native_implementation.resolve())]
        assert native_inputs["cStandard"] == "c17"
        assert native_inputs["cCompileArguments"] == [
            "-DGTI_PROJECT_C_BASE=1",
            "-DGTI_PROJECT_C_PLATFORM=1",
        ]
        assert native_inputs["cppSources"] == [
            str(native_cpp_implementation.resolve())
        ]
        assert native_inputs["compileArguments"] == [
            "-DGTI_PROJECT_NATIVE_BASE=1",
            "-DGTI_PROJECT_NATIVE_PLATFORM=1",
        ]
        assert native_inputs["linkFiles"] == []
        assert native_inputs["orderedLinkOperands"] == []
        assert native_inputs["rawArguments"] == []
        assert not (native_project / "build").exists()

        unusable_native_environment = os.environ.copy()
        unusable_native_environment["CXX"] = "definitely-not-a-native-compiler"
        unusable_native_environment["CC"] = "definitely-not-a-c-compiler"
        checked_native = run(
            [gti, "check", "--verbose"],
            cwd=native_nested,
            env=unusable_native_environment,
        )
        assert "Checked native-project [dev," in checked_native.stdout
        assert not (native_project / "build").exists()

        native_build = run(
            [
                gti,
                "build",
                "--verbose",
                "--cc",
                native_c_compiler,
                "--cxx",
                native_cpp_compiler,
            ],
            cwd=native_nested,
        )
        native_commands = [
            line for line in native_build.stderr.splitlines() if line.startswith("+ ")
        ]
        assert len(native_commands) == 3
        c_command, cpp_command, native_command = native_commands
        c_base_argument = c_command.index("-DGTI_PROJECT_C_BASE=1")
        c_platform_argument = c_command.index("-DGTI_PROJECT_C_PLATFORM=1")
        c_source_argument = c_command.index(str(native_implementation.resolve()))
        assert c_command.startswith(f"+ {native_c_compiler} -std=c17 -O0")
        assert c_base_argument < c_platform_argument < c_source_argument
        assert "GTI_PROJECT_NATIVE_BASE" not in c_command
        assert f'"{native_implementation.resolve()}"' in c_command
        cpp_base_argument = cpp_command.index("-DGTI_PROJECT_NATIVE_BASE=1")
        cpp_platform_argument = cpp_command.index(
            "-DGTI_PROJECT_NATIVE_PLATFORM=1"
        )
        cpp_source_argument = cpp_command.index(
            str(native_cpp_implementation.resolve())
        )
        assert cpp_command.startswith(
            f"+ {native_cpp_compiler} -std=c++23 -O0"
        )
        assert cpp_base_argument < cpp_platform_argument < cpp_source_argument
        assert "GTI_PROJECT_C_BASE" not in cpp_command
        assert f'"{native_cpp_implementation.resolve()}"' in cpp_command
        base_argument = native_command.index("-DGTI_PROJECT_NATIVE_BASE=1")
        platform_argument = native_command.index("-DGTI_PROJECT_NATIVE_PLATFORM=1")
        generated_source = native_command.index(".gti.cpp")
        output_option = native_command.rindex(" -o ")
        assert base_argument < platform_argument < generated_source
        assert "GTI_PROJECT_C_BASE" not in native_command
        assert "--gti-unselected-native-platform" not in native_command
        native_objects = list(
            (native_project / "build/gti/dev").glob(
                "*/.gti-intermediate/*.native-0-native abi.o"
            )
        )
        assert len(native_objects) == 1
        native_object = native_objects[0]
        native_cpp_objects = list(
            (native_project / "build/gti/dev").glob(
                "*/.gti-intermediate/*.native-1-native support.o"
            )
        )
        assert len(native_cpp_objects) == 1
        native_cpp_object = native_cpp_objects[0]
        linked_object = native_command.index(str(native_object.resolve()))
        linked_cpp_object = native_command.index(str(native_cpp_object.resolve()))
        assert generated_source < linked_object < linked_cpp_object < output_option
        assert f'"{native_object.resolve()}"' in native_command
        assert f'"{native_cpp_object.resolve()}"' in native_command
        native_executable = executable_named(
            native_project / "build/gti/dev", "native-project"
        )
        run([str(native_executable)])

        previous_object = native_object.read_bytes()
        native_implementation.write_text("this is not valid C\n", encoding="utf-8")
        failed_c_build = run(
            [gti, "build", "--cc", native_c_compiler],
            expected=1,
            cwd=native_nested,
        )
        assert "native C compiler diagnostics" in failed_c_build.stderr
        assert "native C compiler failed" in failed_c_build.stderr
        assert "generated C++ retained" in failed_c_build.stderr
        assert native_object.read_bytes() == previous_object
        run([str(native_executable)])

        native_implementation.write_text(native_c_contents, encoding="utf-8")
        previous_cpp_object = native_cpp_object.read_bytes()
        native_cpp_implementation.write_text(
            "this is not valid C++\n", encoding="utf-8"
        )
        failed_cpp_build = run(
            [
                gti,
                "build",
                "--cc",
                native_c_compiler,
                "--cxx",
                native_cpp_compiler,
            ],
            expected=1,
            cwd=native_nested,
        )
        assert "native C++ compiler diagnostics" in failed_cpp_build.stderr
        assert "native C++ compiler failed for" in failed_cpp_build.stderr
        assert "generated C++ retained" in failed_cpp_build.stderr
        assert native_cpp_object.read_bytes() == previous_cpp_object
        run([str(native_executable)])

        bad_metadata_format = run(
            [gti, "metadata", "--format", "text"],
            expected=64,
            cwd=check_project,
        )
        assert "metadata format must be json" in bad_metadata_format.stderr

        run_project = root / "run-project"
        run_project.mkdir()
        (run_project / "main.gti").write_text(
            "#include <std/string>\n"
            "#include <std/vector>\n"
            "int main(int argc, std::vector<std::string> argv) { "
            "if (argc != 4 or argv.size() != std::size_t(4)) { return 8; } "
            'if (argv[std::size_t(1)] != "alpha" or '
            'argv[std::size_t(2)] != "two words" or '
            "!argv[std::size_t(3)].empty()) { return 8; } "
            "return 7; }\n",
            encoding="utf-8",
        )
        (run_project / "gti.toml").write_text(
            manifest(
                "[targets.sample]\n"
                'kind = "executable"\n'
                'root = "main.gti"\n'
            ),
            encoding="utf-8",
        )
        ran = run(
            [gti, "run", "--release", "--", "alpha", "two words", ""],
            expected=7,
            cwd=run_project,
        )
        assert "Built sample [release," in ran.stderr
        assert "Running" in ran.stderr

        second_source = project / "src/tool.gti"
        second_source.write_text("int main() { return 0; }\n", encoding="utf-8")
        multiple_targets = targets + (
            "\n[targets.tool]\n"
            'kind = "executable"\n'
            'root = "src/tool.gti"\n'
        )
        manifest_path.write_text(manifest(multiple_targets), encoding="utf-8")
        multiple_metadata = json.loads(
            run([gti, "metadata", "--format", "json"], cwd=nested).stdout
        )
        assert [target["name"] for target in multiple_metadata["targets"]] == [
            "sample",
            "tool",
        ]
        ambiguous = run([gti, "build"], expected=65, cwd=nested)
        assert "error[GTI-B1201]" in ambiguous.stderr
        run([gti, "build", "tool"], cwd=nested)
        run([str(executable_named(project / "build/gti/dev", "tool"))])

        unknown_target = run([gti, "build", "smaple"], expected=65, cwd=project)
        assert "error[GTI-B1200]" in unknown_target.stderr
        assert "Did you mean 'sample'?" in unknown_target.stderr

        manifest_path.write_text(
            manifest(
                "[targets.sample]\n"
                'kind = "executable"\n'
                'rot = "src/main.gti"\n'
            ),
            encoding="utf-8",
        )
        unknown_field = run([gti, "build"], expected=65, cwd=project)
        assert "error[GTI-B1001]" in unknown_field.stderr
        assert "Did you mean 'root'?" in unknown_field.stderr
        assert "rot =" in unknown_field.stderr

        manifest_path.write_text(
            manifest(targets).replace("manifest-version = 1", "manifest-version = 2"),
            encoding="utf-8",
        )
        unsupported = run([gti, "build"], expected=65, cwd=project)
        assert "error[GTI-B1003]" in unsupported.stderr

        outside = root / "outside.gti"
        outside.write_text("int main() { return 0; }\n", encoding="utf-8")
        escaping_targets = (
            "[targets.sample]\n"
            'kind = "executable"\n'
            'root = "../outside.gti"\n'
        )
        manifest_path.write_text(manifest(escaping_targets), encoding="utf-8")
        escaping = run([gti, "build"], expected=65, cwd=project)
        assert "error[GTI-B1104]" in escaping.stderr

        no_project = root / "no-project/deeper"
        no_project.mkdir(parents=True)
        missing = run([gti, "build"], expected=65, cwd=no_project)
        assert "error[GTI-B1100]" in missing.stderr

        unknown_command = run([gti, "buid"], expected=64, cwd=project)
        assert "unknown command 'buid'" in unknown_command.stderr
        run([gti, "build", "--", "-DINVALID=1"], expected=64, cwd=project)

        manifest_path.write_text(manifest(targets, profiles), encoding="utf-8")
        (project / "build/keep.txt").write_text("keep", encoding="utf-8")
        cleaned = run([gti, "clean"], cwd=nested)
        assert "Cleaned" in cleaned.stdout
        assert not (project / "build/gti").exists()
        assert (project / "build/keep.txt").is_file()
        nothing = run([gti, "clean"], cwd=project)
        assert "Nothing to clean" in nothing.stdout

        workspace = root / "workspace"
        app_package = workspace / "packages/app"
        math_package = workspace / "packages/math"
        util_package = workspace / "shared/util"
        (workspace / "src").mkdir(parents=True)
        (app_package / "src").mkdir(parents=True)
        (math_package / "src").mkdir(parents=True)
        (util_package / "src").mkdir(parents=True)
        (workspace / "src/main.gti").write_text(
            "int main() { return 0; }\n", encoding="utf-8"
        )
        (app_package / "src/main.gti").write_text(
            "#include <math/add>\n"
            "int main() { return math::add(2, 3) == 5 ? 0 : 1; }\n",
            encoding="utf-8",
        )
        (math_package / "src/add.gti").write_text(
            "#include <util/value>\n"
            "namespace math {\n"
            "int add(int left, int right) { return left + right + util::zero(); }\n"
            "}\n",
            encoding="utf-8",
        )
        (util_package / "src/value.gti").write_text(
            "namespace util { int zero() { return 0; } }\n",
            encoding="utf-8",
        )
        (workspace / "gti.toml").write_text(
            "manifest-version = 1\n\n"
            "[package]\n"
            'name = "workspace_root"\n'
            'version = "0.1.0"\n\n'
            "[targets.root]\n"
            'kind = "executable"\n'
            'root = "src/main.gti"\n\n'
            "[workspace]\n"
            'members = ["packages/math", "packages/app"]\n',
            encoding="utf-8",
        )
        (app_package / "gti.toml").write_text(
            "manifest-version = 1\n\n"
            "[package]\n"
            'name = "app"\n'
            'version = "1.0.0"\n\n'
            "[dependencies]\n"
            'math = { path = "../math" }\n\n'
            "[targets.app]\n"
            'kind = "executable"\n'
            'root = "src/main.gti"\n',
            encoding="utf-8",
        )
        (math_package / "gti.toml").write_text(
            "manifest-version = 1\n\n"
            "[package]\n"
            'name = "math"\n'
            'version = "2.0.0"\n\n'
            "[dependencies]\n"
            'util = { path = "../../shared/util" }\n',
            encoding="utf-8",
        )
        (util_package / "gti.toml").write_text(
            "manifest-version = 1\n\n"
            "[package]\n"
            'name = "util"\n'
            'version = "3.0.0"\n',
            encoding="utf-8",
        )

        run([gti, "check"], cwd=app_package)
        run([gti, "build", "--package", "app"], cwd=workspace)
        workspace_executable = executable_named(
            workspace / "build/gti/packages/app/dev", "app"
        )
        run([str(workspace_executable)])
        workspace_cached = run(
            [gti, "build", "--package", "app", "--verbose"], cwd=workspace
        )
        assert "gti: cache hit " in workspace_cached.stderr
        workspace_metadata = json.loads(
            run([gti, "metadata", "--package", "app"], cwd=workspace).stdout
        )
        assert workspace_metadata["schemaVersion"] == 8
        assert workspace_metadata["workspace"]["declared"] is True
        assert workspace_metadata["workspace"]["selectedPackage"] == "app"
        assert [
            package["name"] for package in workspace_metadata["workspace"]["packages"]
        ] == ["app", "math", "util", "workspace_root"]
        assert workspace_metadata["workspace"]["packages"][0]["dependencies"] == [
            {"alias": "math", "package": "math@2.0.0"}
        ]
        assert workspace_metadata["workspace"]["packages"][1]["dependencies"] == [
            {"alias": "util", "package": "util@3.0.0"}
        ]
        assert workspace_metadata["workspace"]["packages"][2]["membership"] == (
            "dependency"
        )
        direct_package_include = run(
            [gti, str(app_package / "src/main.gti"), "--emit-cpp"],
            expected=65,
            cwd=app_package,
        )
        assert "error[GTI-I0010]" in direct_package_include.stderr
        unknown_package = run(
            [gti, "build", "--package", "missing"], expected=65, cwd=workspace
        )
        assert "error[GTI-B1607]" in unknown_package.stderr
        source_only_package = run(
            [gti, "build", "--package", "math"], expected=65, cwd=workspace
        )
        assert "error[GTI-B1201]" in source_only_package.stderr
        assert "source-only" in source_only_package.stderr
        run([gti, "clean"], cwd=app_package)
        assert not (workspace / "build/gti").exists()

        redirected_workspace = root / "redirected-workspace"
        redirected_member = redirected_workspace / "member"
        redirected_external = root / "redirected-external"
        redirected_member.mkdir(parents=True)
        redirected_external.mkdir()
        (redirected_workspace / "gti.toml").write_text(
            "manifest-version = 1\n\n"
            "[package]\n"
            'name = "redirected_workspace"\n'
            'version = "1.0.0"\n\n'
            "[workspace]\n"
            'members = ["member"]\n',
            encoding="utf-8",
        )
        (redirected_external / "gti.toml").write_text(
            "manifest-version = 1\n\n"
            "[package]\n"
            'name = "redirected_external"\n'
            'version = "1.0.0"\n\n'
            "[targets.redirected_external]\n"
            'kind = "executable"\n'
            'root = "main.gti"\n',
            encoding="utf-8",
        )
        (redirected_external / "main.gti").write_text(
            "int main() { return 0; }\n", encoding="utf-8"
        )
        external_build = redirected_external / "build/gti"
        external_build.mkdir(parents=True)
        external_sentinel = external_build / "sentinel"
        external_sentinel.write_text("preserve", encoding="utf-8")
        try:
            (redirected_member / "gti.toml").symlink_to(
                redirected_external / "gti.toml"
            )
        except OSError:
            pass
        else:
            for command in (["metadata"], ["build"], ["clean"]):
                rejected_redirect = run(
                    [gti, *command], expected=65, cwd=redirected_member
                )
                assert "error[GTI-B1101]" in rejected_redirect.stderr
                assert "must not be a symbolic link" in rejected_redirect.stderr
                assert external_sentinel.read_text(encoding="utf-8") == "preserve"
            assert not (redirected_workspace / "build/gti").exists()

        example_source = (
            pathlib.Path(__file__).resolve().parent.parent
            / "examples/transit-planner"
        )
        example_project = root / "transit-planner"
        shutil.copytree(example_source, example_project)
        example_metadata = json.loads(
            run(
                [gti, "metadata", "--format", "json"],
                cwd=example_project / "src",
            ).stdout
        )
        assert example_metadata["package"]["name"] == "transit-planner"
        assert example_metadata["profiles"][1]["optimization"] == 3
        run([gti, "check"], cwd=example_project)
        example_run = run([gti, "run", "--release"], cwd=example_project)
        assert example_run.stdout == (
            "GTI Transit Planner\n"
            "Loaded 12 bidirectional links from data/network.txt\n"
            "\n"
            "Fastest route:\n"
            "Depot -> Museum -> Market -> University -> Stadium -> Observatory\n"
            "Total travel time: 11 minutes\n"
            "Stations visited: 6\n"
        )
        assert "Built transit-planner [release," in example_run.stderr
        assert "Running" in example_run.stderr
        (example_project / "data/network.txt").write_text(
            "0 invalid 2\n", encoding="utf-8"
        )
        invalid_example = run(
            [gti, "run", "--release"], expected=1, cwd=example_project
        )
        assert invalid_example.stdout == (
            "Network loading failed:\n"
            "the network file contains an invalid character\n"
        )
        run([gti, "clean"], cwd=example_project / "src")
        assert not (example_project / "build/gti").exists()

        engine_source = (
            pathlib.Path(__file__).resolve().parent.parent
            / "examples/game-engine/gti"
        )
        engine_project = root / "psych-core-example"
        shutil.copytree(
            engine_source,
            engine_project,
            ignore=shutil.ignore_patterns("build"),
        )
        engine_metadata = json.loads(
            run([gti, "metadata", "--format", "json"], cwd=engine_project).stdout
        )
        assert engine_metadata["package"]["name"] == "psych-core-example"
        assert {target["name"] for target in engine_metadata["targets"]} == {
            "layer-stack-tests",
            "psych-core",
        }
        run([gti, "check", "psych-core"], cwd=engine_project)
        engine_tests = run([gti, "test", "--release"], cwd=engine_project)
        assert "layer-stack-tests" in engine_tests.stderr
        engine_run = run(
            [gti, "run", "psych-core", "--release"], cwd=engine_project
        )
        assert engine_run.stdout == (
            "Psych CLI game\n"
            "----------------\n"
            "status overlay attached\n"
            "game layer attached\n"
            "application started through psych::Application\n"
            "turn 1: the player enters the ruins\n"
            "status overlay: frame presented\n"
            "turn 2: the player finds a brass key\n"
            "status overlay: frame presented\n"
            "turn 3: the player opens the old gate\n"
            "status overlay: frame presented\n"
            "game layer detached\n"
            "status overlay detached\n"
            "application stopped\n"
            "----------------\n"
            "session completed successfully\n"
        )
        assert "Built psych-core [release," in engine_run.stderr
        run([gti, "clean"], cwd=engine_project)
        assert not (engine_project / "build/gti").exists()

        # --- Locked git dependencies ---
        git = shutil.which("git")
        if git is None:
            raise AssertionError("git dependency tests require git on PATH")

        def git_in(repository, *arguments):
            return run(
                [
                    git,
                    "-C",
                    str(repository),
                    "-c",
                    "user.email=gti@test",
                    "-c",
                    "user.name=gti",
                    *arguments,
                ]
            )

        def make_upstream(path, name, add_bias):
            (path / "src").mkdir(parents=True)
            (path / "gti.toml").write_text(
                "manifest-version = 1\n\n"
                "[package]\n"
                f'name = "{name}"\n'
                'version = "0.1.0"\n',
                encoding="utf-8",
            )
            (path / "src/add.gti").write_text(
                f"namespace {name} {{\n"
                f"int add(int left, int right) {{ return left + right"
                f" + {add_bias}; }}\n"
                "}\n",
                encoding="utf-8",
            )
            run([git, "init", "-q", str(path)])
            git_in(path, "add", "-A")
            git_in(path, "commit", "-qm", "initial")
            return git_in(path, "rev-parse", "HEAD").stdout.strip()

        upstream = root / "git-upstream"
        first_revision = make_upstream(upstream, "mathdep", 0)

        git_app = root / "git-app"
        (git_app / "src").mkdir(parents=True)

        def write_app_manifest(revision):
            (git_app / "gti.toml").write_text(
                "manifest-version = 1\n\n"
                "[package]\n"
                'name = "git-app"\n'
                'version = "0.1.0"\n\n'
                "[dependencies]\n"
                f'mathdep = {{ git = "{upstream}", rev = "{revision}" }}\n\n'
                "[targets.git-app]\n"
                'kind = "executable"\n'
                'root = "src/main.gti"\n',
                encoding="utf-8",
            )

        write_app_manifest(first_revision)
        (git_app / "src/main.gti").write_text(
            "#include <mathdep/add>\n\n"
            "int main() {\n"
            "  return mathdep::add(2, 3) == 5 ? 0 : 1;\n"
            "}\n",
            encoding="utf-8",
        )

        # A build without gti.lock fails closed with guidance.
        unlocked_build = run([gti, "build"], expected=65, cwd=git_app)
        assert "error[GTI-B1701]" in unlocked_build.stderr
        assert "Run `gti fetch`" in unlocked_build.stderr

        # Fetch acquires the pinned revision and writes a deterministic lock.
        fetched = run([gti, "fetch"], cwd=git_app)
        assert f"Locked mathdep@0.1.0 git+{upstream}#{first_revision}" in (
            fetched.stdout
        )
        assert "Wrote" in fetched.stdout
        lock_path = git_app / "gti.lock"
        lock_text = lock_path.read_text(encoding="utf-8")
        assert "lock-version = 1" in lock_text
        assert f'rev = "{first_revision}"' in lock_text
        assert 'checksum = "sha256:' in lock_text
        refetched = run([gti, "fetch"], cwd=git_app)
        assert "gti.lock is up to date (1 git dependency)" in refetched.stdout
        assert lock_path.read_text(encoding="utf-8") == lock_text

        run([gti, "build"], cwd=git_app)
        run([gti, "run"], cwd=git_app)

        # Metadata reports the resolved source without acquiring anything.
        git_metadata = json.loads(run([gti, "metadata"], cwd=git_app).stdout)
        source_by_name = {
            package["name"]: package["source"]
            for package in git_metadata["workspace"]["packages"]
        }
        assert source_by_name["git-app"] == "path"
        assert source_by_name["mathdep"].startswith(f"git+{upstream}#")
        assert f"#{first_revision}#sha256:" in source_by_name["mathdep"]

        # Upstream advancing cannot change a pinned build.
        (upstream / "src/add.gti").write_text(
            "namespace mathdep {\n"
            "int add(int left, int right) { return left + right + 100; }\n"
            "}\n",
            encoding="utf-8",
        )
        git_in(upstream, "commit", "-qam", "bias")
        second_revision = git_in(upstream, "rev-parse", "HEAD").stdout.strip()
        run([gti, "run"], cwd=git_app)

        # Re-pinning without fetch is a stale lock; --locked refuses too.
        write_app_manifest(second_revision)
        stale_build = run([gti, "build"], expected=65, cwd=git_app)
        assert "error[GTI-B1701]" in stale_build.stderr
        assert "gti.lock does not record" in stale_build.stderr
        locked_build = run([gti, "build", "--locked"], expected=65, cwd=git_app)
        assert "error[GTI-B1701]" in locked_build.stderr

        # Fetch updates the lock and the new revision's behavior lands.
        run([gti, "fetch"], cwd=git_app)
        biased_run = run([gti, "run"], expected=1, cwd=git_app)
        assert "Running" in biased_run.stderr
        second_lock_text = lock_path.read_text(encoding="utf-8")
        assert f'rev = "{second_revision}"' in second_lock_text

        # Clean removes the store; --offline then fails closed while a plain
        # build re-acquires from the lock without touching gti.lock.
        run([gti, "clean"], cwd=git_app)
        offline_build = run(
            [gti, "build", "--offline"], expected=65, cwd=git_app
        )
        assert "error[GTI-B1703]" in offline_build.stderr
        run([gti, "build"], cwd=git_app)
        assert lock_path.read_text(encoding="utf-8") == second_lock_text
        run([gti, "build", "--offline"], cwd=git_app)
        offline_check = run([gti, "check", "--offline"], cwd=git_app)
        assert "Checked git-app" in offline_check.stdout

        # A tampered stored tree is refused before source loading, and fetch
        # restores the true content without laundering the lock checksum.
        checkout_files = sorted(
            (git_app / "build/gti/deps/git/checkouts").rglob("add.gti")
        )
        assert checkout_files, "expected a materialized checkout"
        tampered = checkout_files[0]
        tampered.write_text(
            tampered.read_text(encoding="utf-8") + "// tampered\n",
            encoding="utf-8",
        )
        tampered_build = run([gti, "build"], expected=65, cwd=git_app)
        assert "error[GTI-B1704]" in tampered_build.stderr
        assert "refusing to load unverified source" in tampered_build.stderr
        run([gti, "fetch"], cwd=git_app)
        assert lock_path.read_text(encoding="utf-8") == second_lock_text
        assert "tampered" not in tampered.read_text(encoding="utf-8")
        run([gti, "build"], cwd=git_app)

        # A tampered lock checksum is also refused.
        checksum_marker = 'checksum = "sha256:'
        digit_index = second_lock_text.index(checksum_marker) + len(
            checksum_marker
        )
        flipped_digit = "0" if second_lock_text[digit_index] != "0" else "1"
        lock_path.write_text(
            second_lock_text[:digit_index]
            + flipped_digit
            + second_lock_text[digit_index + 1 :],
            encoding="utf-8",
        )
        forged_build = run([gti, "build"], expected=65, cwd=git_app)
        assert "error[GTI-B1704]" in forged_build.stderr
        lock_path.write_text("lock-version = 1\nnot valid [", encoding="utf-8")
        malformed_lock = run([gti, "build"], expected=65, cwd=git_app)
        assert "error[GTI-B1702]" in malformed_lock.stderr
        run([gti, "fetch"], cwd=git_app)
        run([gti, "build"], cwd=git_app)

        # fetch --offline verifies from the local store and fails honestly
        # when the store is gone.
        offline_fetch = run([gti, "fetch", "--offline"], cwd=git_app)
        assert "gti.lock is up to date" in offline_fetch.stdout
        run([gti, "clean"], cwd=git_app)
        empty_offline_fetch = run(
            [gti, "fetch", "--offline"], expected=65, cwd=git_app
        )
        assert "error[GTI-B1705]" in empty_offline_fetch.stderr
        assert "offline" in empty_offline_fetch.stderr

        # Trees carrying symbolic links or submodules are rejected rather
        # than extracted.
        linked_upstream = root / "git-linked"
        make_upstream(linked_upstream, "linkdep", 0)
        os.symlink("src/add.gti", linked_upstream / "alias.gti")
        git_in(linked_upstream, "add", "-A")
        git_in(linked_upstream, "commit", "-qm", "link")
        linked_revision = git_in(
            linked_upstream, "rev-parse", "HEAD"
        ).stdout.strip()
        linked_app = root / "git-linked-app"
        (linked_app / "src").mkdir(parents=True)
        (linked_app / "gti.toml").write_text(
            "manifest-version = 1\n\n"
            "[package]\n"
            'name = "linked-app"\n'
            'version = "0.1.0"\n\n'
            "[dependencies]\n"
            f'linkdep = {{ git = "{linked_upstream}", rev = '
            f'"{linked_revision}" }}\n\n'
            "[targets.linked-app]\n"
            'kind = "executable"\n'
            'root = "src/main.gti"\n',
            encoding="utf-8",
        )
        (linked_app / "src/main.gti").write_text(
            "int main() { return 0; }\n", encoding="utf-8"
        )
        rejected_link = run([gti, "fetch"], expected=65, cwd=linked_app)
        assert "error[GTI-B1705]" in rejected_link.stderr
        assert "symbolic link" in rejected_link.stderr

        gitlink_upstream = root / "git-gitlink"
        make_upstream(gitlink_upstream, "subdep", 0)
        git_in(
            gitlink_upstream,
            "update-index",
            "--add",
            "--cacheinfo",
            f"160000,{first_revision},vendored",
        )
        git_in(gitlink_upstream, "commit", "-qm", "gitlink")
        gitlink_revision = git_in(
            gitlink_upstream, "rev-parse", "HEAD"
        ).stdout.strip()
        (linked_app / "gti.toml").write_text(
            "manifest-version = 1\n\n"
            "[package]\n"
            'name = "linked-app"\n'
            'version = "0.1.0"\n\n'
            "[dependencies]\n"
            f'subdep = {{ git = "{gitlink_upstream}", rev = '
            f'"{gitlink_revision}" }}\n\n'
            "[targets.linked-app]\n"
            'kind = "executable"\n'
            'root = "src/main.gti"\n',
            encoding="utf-8",
        )
        (linked_app / "gti.lock").unlink(missing_ok=True)
        rejected_gitlink = run([gti, "fetch"], expected=65, cwd=linked_app)
        assert "error[GTI-B1705]" in rejected_gitlink.stderr
        assert "submodule" in rejected_gitlink.stderr

        # Transitive pinned git dependencies lock as one closure with
        # recorded dependency edges.
        middle_upstream = root / "git-middle"
        (middle_upstream / "src").mkdir(parents=True)
        (middle_upstream / "gti.toml").write_text(
            "manifest-version = 1\n\n"
            "[package]\n"
            'name = "middledep"\n'
            'version = "0.2.0"\n\n'
            "[dependencies]\n"
            f'mathdep = {{ git = "{upstream}", rev = "{second_revision}" }}\n',
            encoding="utf-8",
        )
        (middle_upstream / "src/twice.gti").write_text(
            "#include <mathdep/add>\n\n"
            "namespace middledep {\n"
            "int twice(int value) { return mathdep::add(value, value); }\n"
            "}\n",
            encoding="utf-8",
        )
        run([git, "init", "-q", str(middle_upstream)])
        git_in(middle_upstream, "add", "-A")
        git_in(middle_upstream, "commit", "-qm", "initial")
        middle_revision = git_in(
            middle_upstream, "rev-parse", "HEAD"
        ).stdout.strip()

        chained_app = root / "git-chained-app"
        (chained_app / "src").mkdir(parents=True)
        (chained_app / "gti.toml").write_text(
            "manifest-version = 1\n\n"
            "[package]\n"
            'name = "chained-app"\n'
            'version = "0.1.0"\n\n'
            "[dependencies]\n"
            f'middledep = {{ git = "{middle_upstream}", rev = '
            f'"{middle_revision}" }}\n\n'
            "[targets.chained-app]\n"
            'kind = "executable"\n'
            'root = "src/main.gti"\n',
            encoding="utf-8",
        )
        (chained_app / "src/main.gti").write_text(
            "#include <middledep/twice>\n\n"
            "int main() {\n"
            "  return middledep::twice(3) == 106 ? 0 : 1;\n"
            "}\n",
            encoding="utf-8",
        )
        chained_fetch = run([gti, "fetch"], cwd=chained_app)
        assert "Locked mathdep@0.1.0" in chained_fetch.stdout
        assert "Locked middledep@0.2.0" in chained_fetch.stdout
        chained_lock = (chained_app / "gti.lock").read_text(encoding="utf-8")
        assert 'dependencies = ["mathdep"]' in chained_lock
        run([gti, "build"], cwd=chained_app)
        run([gti, "run"], cwd=chained_app)

        # No-git projects need no lock.
        no_git_fetch = run([gti, "fetch"], cwd=new_project)
        assert "No git dependencies to lock" in no_git_fetch.stdout
        assert not (new_project / "gti.lock").exists()


        # --- Native-source whole-program caching ---
        cache_cc = shutil.which("cc")
        cache_cxx = shutil.which("c++")
        if cache_cc is None or cache_cxx is None:
            raise AssertionError("native cache tests require cc and c++ on PATH")
        cache_native = root / "cache-native"
        (cache_native / "src").mkdir(parents=True)
        (cache_native / "native/include").mkdir(parents=True)
        (cache_native / "native/lib").mkdir(parents=True)

        def cache_native_manifest(native_lines):
            (cache_native / "gti.toml").write_text(
                "manifest-version = 1\n\n"
                "[package]\n"
                'name = "cache-native"\n'
                'version = "0.1.0"\n\n'
                "[package.native]\n"
                + "".join(line + "\n" for line in native_lines)
                + "\n[targets.cache-native]\n"
                'kind = "executable"\n'
                'root = "src/main.gti"\n',
                encoding="utf-8",
            )

        (cache_native / "native/include/support.h").write_text(
            "#define NATIVE_BIAS 0\n"
            "int native_add(int left, int right);\n",
            encoding="utf-8",
        )
        (cache_native / "native/support.c").write_text(
            '#include "support.h"\n'
            "int native_add(int left, int right) {\n"
            "  return left + right + NATIVE_BIAS;\n"
            "}\n",
            encoding="utf-8",
        )
        (cache_native / "src/main.gti").write_text(
            'extern "C" {\n'
            "int native_add(int left, int right);\n"
            "}\n\n"
            "int main() {\n"
            "  return native_add(2, 3) == 5 ? 0 : 1;\n"
            "}\n",
            encoding="utf-8",
        )
        link_object = cache_native / "native/lib/extra.o"
        run(
            [
                cache_cc,
                "-c",
                str(cache_native / "native/support.c"),
                "-I",
                str(cache_native / "native/include"),
                "-o",
                str(link_object),
            ]
        )

        def cache_native_build(expected=0, extra=(), env=None):
            return run(
                [
                    gti,
                    "build",
                    "--verbose",
                    "--cc",
                    cache_cc,
                    "--cxx",
                    cache_cxx,
                    *extra,
                ],
                expected=expected,
                cwd=cache_native,
                env=env,
            )

        def cache_key(report):
            for line in report.stderr.splitlines():
                for status in ("gti: cache miss ", "gti: cache hit "):
                    if line.startswith(status):
                        return line[len(status):].split(" ", 1)[0]
            raise AssertionError(f"no cache key in stderr: {report.stderr}")

        cache_native_manifest(
            [
                'include-dirs = ["native/include"]',
                'c-sources = ["native/support.c"]',
            ]
        )
        native_cache_first = cache_native_build()
        assert "gti: cache miss " in native_cache_first.stderr
        first_native_key = cache_key(native_cache_first)
        native_cache_second = cache_native_build()
        assert "gti: cache hit " in native_cache_second.stderr
        assert cache_key(native_cache_second) == first_native_key
        assert not any(
            line.startswith("+ ")
            for line in native_cache_second.stderr.splitlines()
        )
        run([gti, "run"], cwd=cache_native)

        # A header added beside the C source shadows the declared include
        # directory through quote-include search order. The cache must miss
        # and rebuild with the shadowing header rather than restore the
        # previous executable.
        shadow_header = cache_native / "native/support.h"
        shadow_header.write_text(
            "#define NATIVE_BIAS 1\n"
            "int native_add(int left, int right);\n",
            encoding="utf-8",
        )
        shadowed_build = cache_native_build()
        assert "gti: cache miss " in shadowed_build.stderr
        assert cache_key(shadowed_build) != first_native_key
        run([gti, "run"], expected=1, cwd=cache_native)
        shadow_header.unlink()
        unshadowed_build = cache_native_build()
        assert "gti: cache hit " in unshadowed_build.stderr
        assert cache_key(unshadowed_build) == first_native_key
        run([gti, "run"], cwd=cache_native)

        # Changing a depfile-discovered header invalidates the identity.
        (cache_native / "native/include/support.h").write_text(
            "#define NATIVE_BIAS 0\n"
            "int native_add(int left, int right);\n"
            "/* revised */\n",
            encoding="utf-8",
        )
        revised_header_build = cache_native_build()
        assert "gti: cache miss " in revised_header_build.stderr
        assert cache_key(revised_header_build) != first_native_key

        # A content-complete relocatable object as an exact link file joins
        # the identity instead of bypassing.
        cache_native_manifest(
            [
                'include-dirs = ["native/include"]',
                'link-files = ["native/lib/extra.o"]',
            ]
        )
        link_file_first = cache_native_build()
        assert "gti: cache miss " in link_file_first.stderr
        link_file_second = cache_native_build()
        assert "gti: cache hit " in link_file_second.stderr

        # Every remaining bypass reason is reported explicitly.
        shared_library = cache_native / (
            "native/lib/libextra_shared."
            + ("dylib" if sys.platform == "darwin" else "so")
        )
        run(
            [
                cache_cc,
                "-shared",
                "-fPIC",
                str(cache_native / "native/support.c"),
                "-I",
                str(cache_native / "native/include"),
                "-o",
                str(shared_library),
            ]
        )
        cache_native_manifest(
            [
                'include-dirs = ["native/include"]',
                f'link-files = ["native/lib/{shared_library.name}"]',
            ]
        )
        shared_bypass = cache_native_build()
        assert "gti: cache bypassed" in shared_bypass.stderr
        assert (
            "not a content-complete archive or object" in shared_bypass.stderr
        )

        cache_native_manifest(
            [
                'include-dirs = ["native/include"]',
                'c-sources = ["native/support.c"]',
                'c-compile-args = ["-DNATIVE_EXTRA=1"]',
            ]
        )
        opaque_bypass = cache_native_build()
        assert "gti: cache bypassed" in opaque_bypass.stderr
        assert "opaque native argument vectors" in opaque_bypass.stderr

        cache_native_manifest(
            [
                'include-dirs = ["native/include"]',
                'c-sources = ["native/support.c"]',
                'libraries = ["m"]',
            ]
        )
        named_bypass = cache_native_build()
        assert "gti: cache bypassed" in named_bypass.stderr
        assert "name-resolved libraries and frameworks" in named_bypass.stderr

        cache_native_manifest(
            [
                'include-dirs = ["native/include"]',
                'c-sources = ["native/support.c"]',
                'library-dirs = ["native/lib"]',
            ]
        )
        library_dir_bypass = cache_native_build()
        assert "gti: cache bypassed" in library_dir_bypass.stderr
        assert "library search directories" in library_dir_bypass.stderr

        cache_native_manifest(
            [
                'include-dirs = ["native/include"]',
                'c-sources = ["native/support.c"]',
            ]
        )
        injected_environment = os.environ.copy()
        injected_environment["CPATH"] = str(cache_native / "native/include")
        environment_bypass = cache_native_build(env=injected_environment)
        assert "gti: cache bypassed" in environment_bypass.stderr
        assert "environment search paths" in environment_bypass.stderr

        time_macro_source = cache_native / "native/support.c"
        time_macro_source.write_text(
            '#include "support.h"\n'
            "int native_add(int left, int right) {\n"
            "  return left + right + NATIVE_BIAS + (__DATE__[0] == 0);\n"
            "}\n",
            encoding="utf-8",
        )
        time_macro_bypass = cache_native_build()
        assert "gti: cache bypassed" in time_macro_bypass.stderr
        assert "time-and-date preprocessor macros" in time_macro_bypass.stderr
        time_macro_source.write_text(
            '#include "support.h"\n'
            "int native_add(int left, int right) {\n"
            "  return left + right + NATIVE_BIAS;\n"
            "}\n",
            encoding="utf-8",
        )

        # Malformed and partial cache entries are diagnosed, never executed,
        # and are replaced only after a successful rebuild.
        rebuilt = cache_native_build()
        assert "gti: cache " in rebuilt.stderr
        entry_directory = (
            cache_native / "build/gti/cache/v2" / cache_key(rebuilt)
        )
        if "gti: cache miss " in rebuilt.stderr:
            assert "gti: cache hit " in cache_native_build().stderr
        assert entry_directory.is_dir()
        (entry_directory / "executable").write_bytes(b"corrupt")
        corrupt_payload = cache_native_build()
        assert "ignored corrupt build cache entry" in corrupt_payload.stderr
        assert "gti: cache recovered " in corrupt_payload.stderr
        run([gti, "run"], cwd=cache_native)
        (entry_directory / "generated.cpp").unlink()
        partial_entry = cache_native_build()
        assert "ignored corrupt build cache entry" in partial_entry.stderr
        assert "gti: cache recovered " in partial_entry.stderr
        (entry_directory / "metadata").write_text(
            "not a cache metadata document\n", encoding="utf-8"
        )
        malformed_metadata = cache_native_build()
        assert "ignored corrupt build cache entry" in malformed_metadata.stderr
        assert "gti: cache recovered " in malformed_metadata.stderr
        assert "gti: cache hit " in cache_native_build().stderr

        # --- Multi-target parallel builds ---
        multi_project = root / "multi-target"
        (multi_project / "src").mkdir(parents=True)
        (multi_project / "tests").mkdir(parents=True)
        multi_targets = ["alpha", "beta", "gamma"]
        manifest_lines = [
            "manifest-version = 1",
            "",
            "[package]",
            'name = "multi-target"',
            'version = "0.1.0"',
            "",
        ]
        for name in multi_targets:
            (multi_project / "src" / f"{name}.gti").write_text(
                "int main() { return 0; }\n", encoding="utf-8"
            )
            manifest_lines += [
                f"[targets.{name}]",
                'kind = "executable"',
                f'root = "src/{name}.gti"',
                "",
            ]
        (multi_project / "tests/zulu.gti").write_text(
            "int main() { return 0; }\n", encoding="utf-8"
        )
        manifest_lines += [
            "[targets.zulu-check]",
            'kind = "test"',
            'root = "tests/zulu.gti"',
        ]
        (multi_project / "gti.toml").write_text(
            "\n".join(manifest_lines) + "\n", encoding="utf-8"
        )
        all_target_names = multi_targets + ["zulu-check"]

        def multi_artifact_bytes():
            triple_directories = [
                entry
                for entry in (multi_project / "build/gti/dev").iterdir()
                if entry.is_dir()
            ]
            assert len(triple_directories) == 1
            return {
                name: (
                    triple_directories[0]
                    / (f"{name}.exe" if sys.platform == "win32" else name)
                ).read_bytes()
                for name in all_target_names
            }

        parallel_all = run(
            [gti, "build", "--all", "--jobs", "4", "--no-cache"],
            cwd=multi_project,
        )
        built_positions = [
            parallel_all.stdout.index(f"Built {name} [dev,")
            for name in all_target_names
        ]
        assert built_positions == sorted(built_positions)
        assert "Built 4 targets [dev]" in parallel_all.stdout
        parallel_artifacts = multi_artifact_bytes()

        run([gti, "clean"], cwd=multi_project)
        serial_all = run(
            [gti, "build", "--all", "--jobs", "1", "--no-cache"],
            cwd=multi_project,
        )
        assert serial_all.stdout == parallel_all.stdout
        assert serial_all.stderr == parallel_all.stderr
        serial_artifacts = multi_artifact_bytes()
        assert serial_artifacts == parallel_artifacts

        # A cached rebuild restores every target through the same command.
        cached_all = run([gti, "build", "--all"], cwd=multi_project)
        assert "Built 4 targets [dev]" in cached_all.stdout

        # One failing target must produce the same ordered diagnostics and
        # the same first-failure status regardless of scheduling.
        (multi_project / "src/delta.gti").write_text(
            "int main() { return undeclared_symbol; }\n", encoding="utf-8"
        )
        failing_manifest = (multi_project / "gti.toml").read_text(
            encoding="utf-8"
        ) + (
            "\n[targets.delta]\n"
            'kind = "executable"\n'
            'root = "src/delta.gti"\n'
        )
        (multi_project / "gti.toml").write_text(
            failing_manifest, encoding="utf-8"
        )
        parallel_failure = run(
            [gti, "build", "--all", "--jobs", "4", "--no-cache"],
            expected=65,
            cwd=multi_project,
        )
        assert (
            "gti: build --all: target 'delta' failed with exit code 65"
            in parallel_failure.stderr
        )
        for name in all_target_names:
            assert f"Built {name} [dev," in parallel_failure.stdout
        run([gti, "clean"], cwd=multi_project)
        serial_failure = run(
            [gti, "build", "--all", "--jobs", "1", "--no-cache"],
            expected=65,
            cwd=multi_project,
        )
        assert serial_failure.stdout == parallel_failure.stdout
        assert serial_failure.stderr == parallel_failure.stderr

        # Option surface errors stay focused usage failures.
        all_with_target = run(
            [gti, "build", "--all", "alpha"], expected=64, cwd=multi_project
        )
        assert "--all cannot be combined with a target selection" in (
            all_with_target.stderr
        )
        jobs_without_all = run(
            [gti, "build", "--jobs", "2"], expected=64, cwd=multi_project
        )
        assert "--jobs requires --all" in jobs_without_all.stderr
        invalid_jobs = run(
            [gti, "build", "--all", "--jobs", "0"],
            expected=64,
            cwd=multi_project,
        )
        assert "--jobs requires a positive whole number" in invalid_jobs.stderr
        all_on_check = run(
            [gti, "check", "--all"], expected=64, cwd=multi_project
        )
        assert "--all is not supported by gti check" in all_on_check.stderr


        # --- Native dependency composition ---
        wrapper = root / "native-wrapper"
        (wrapper / "src").mkdir(parents=True)
        (wrapper / "native/include").mkdir(parents=True)

        def write_wrapper_native(native_lines):
            (wrapper / "gti.toml").write_text(
                "manifest-version = 1\n\n"
                "[package]\n"
                'name = "wrapper"\n'
                'version = "0.1.0"\n\n'
                "[package.native]\n" + native_lines,
                encoding="utf-8",
            )

        write_wrapper_native(
            'include-dirs = ["native/include"]\n'
            'c-sources = ["native/impl.c"]\n'
            'c-compile-args = ["-DWRAP_BIAS=7"]\n'
        )
        (wrapper / "native/include/wrap.h").write_text(
            "int wrap_add(int left, int right);\n", encoding="utf-8"
        )
        (wrapper / "native/impl.c").write_text(
            '#include "wrap.h"\n'
            "int wrap_add(int left, int right) {\n"
            "  return left + right + WRAP_BIAS;\n"
            "}\n",
            encoding="utf-8",
        )
        (wrapper / "src/wrap.gti").write_text(
            'extern "C" {\n'
            "int wrap_add(int left, int right);\n"
            "}\n\n"
            "namespace wrap {\n"
            "int add(int left, int right) { return wrap_add(left, right); }\n"
            "}\n",
            encoding="utf-8",
        )

        composed_app = root / "composed-app"
        (composed_app / "src").mkdir(parents=True)
        (composed_app / "gti.toml").write_text(
            "manifest-version = 1\n\n"
            "[package]\n"
            'name = "composed-app"\n'
            'version = "0.1.0"\n\n'
            "[dependencies]\n"
            'wrap = { path = "../native-wrapper" }\n\n'
            "[targets.composed-app]\n"
            'kind = "executable"\n'
            'root = "src/main.gti"\n',
            encoding="utf-8",
        )
        (composed_app / "src/main.gti").write_text(
            "#include <wrap/wrap>\n\n"
            "int main() {\n"
            "  return wrap::add(2, 3) == 12 ? 0 : 1;\n"
            "}\n",
            encoding="utf-8",
        )

        # A dependency's contained native contract composes: its C source
        # compiles with only its own include directories and validated
        # macros, and the program observes the dependency's behavior.
        composed_build = run(
            [gti, "build", "--verbose"], cwd=composed_app
        )
        assert (
            "dependency native inputs require compiler dependency discovery"
            in composed_build.stderr
        )
        wrapper_compile = next(
            line
            for line in composed_build.stderr.splitlines()
            if line.startswith("+ ") and "impl.c" in line
        )
        assert "-DWRAP_BIAS=7" in wrapper_compile
        assert str(wrapper / "native/include") in wrapper_compile
        run([gti, "run"], cwd=composed_app)

        # Composition is scoped: the application's own native sources do not
        # see dependency include directories.
        (composed_app / "native").mkdir()
        (composed_app / "native/leak.c").write_text(
            '#include "wrap.h"\n'
            "int leak_probe(void) { return wrap_add(1, 1); }\n",
            encoding="utf-8",
        )
        leak_manifest = (composed_app / "gti.toml").read_text(
            encoding="utf-8"
        ).replace(
            "[dependencies]",
            '[package.native]\nc-sources = ["native/leak.c"]\n\n[dependencies]',
        )
        (composed_app / "gti.toml").write_text(leak_manifest, encoding="utf-8")
        leak_build = run([gti, "build"], expected=1, cwd=composed_app)
        assert "wrap.h" in leak_build.stderr
        assert "native C compiler failed" in leak_build.stderr
        (composed_app / "gti.toml").write_text(
            leak_manifest.replace(
                '[package.native]\nc-sources = ["native/leak.c"]\n\n', ""
            ),
            encoding="utf-8",
        )

        # Metadata publishes the composed groups without building.
        composed_metadata = json.loads(
            run([gti, "metadata"], cwd=composed_app).stdout
        )
        composed_native = composed_metadata["targets"][0]["outputs"][0][
            "native"
        ]
        assert composed_metadata["schemaVersion"] == 8
        assert [
            (group["package"], group["cMacroDefinitions"])
            for group in composed_native["dependencyNative"]
        ] == [("wrapper@0.1.0", ["-DWRAP_BIAS=7"])]

        # Opaque argument vectors never compose from a dependency.
        write_wrapper_native(
            'c-sources = ["native/impl.c"]\n'
            'c-compile-args = ["-DWRAP_BIAS=7"]\n'
            'link-args = ["-Wl,-S"]\n'
        )
        rejected_link_args = run(
            [gti, "build"], expected=65, cwd=composed_app
        )
        assert "error[GTI-B1606]" in rejected_link_args.stderr
        assert "linker or raw argument" in rejected_link_args.stderr
        write_wrapper_native(
            'c-sources = ["native/impl.c"]\n'
            'c-compile-args = ["-DWRAP_BIAS=7", "-fcommon"]\n'
        )
        rejected_flag = run([gti, "build"], expected=65, cwd=composed_app)
        assert "error[GTI-B1606]" in rejected_flag.stderr
        assert "'-fcommon'" in rejected_flag.stderr
        assert "-D<name>[=<value>]" in rejected_flag.stderr
        write_wrapper_native(
            'include-dirs = ["native/include"]\n'
            'c-sources = ["native/impl.c"]\n'
            'c-compile-args = ["-DWRAP_BIAS=7"]\n'
        )
        run([gti, "run"], cwd=composed_app)

        # Transitive composition links dependents before dependencies.
        base_package = root / "native-base"
        (base_package / "src").mkdir(parents=True)
        (base_package / "native").mkdir()
        (base_package / "gti.toml").write_text(
            "manifest-version = 1\n\n"
            "[package]\n"
            'name = "basedep"\n'
            'version = "0.1.0"\n\n'
            "[package.native]\n"
            'c-sources = ["native/base.c"]\n',
            encoding="utf-8",
        )
        (base_package / "native/base.c").write_text(
            "int base_value(void) { return 40; }\n", encoding="utf-8"
        )
        (base_package / "src/decl.gti").write_text("\n", encoding="utf-8")

        middle_package = root / "native-middle"
        (middle_package / "src").mkdir(parents=True)
        (middle_package / "native").mkdir()
        (middle_package / "gti.toml").write_text(
            "manifest-version = 1\n\n"
            "[package]\n"
            'name = "middledep"\n'
            'version = "0.1.0"\n\n'
            "[package.native]\n"
            'c-sources = ["native/middle.c"]\n\n'
            "[dependencies]\n"
            'basedep = { path = "../native-base" }\n',
            encoding="utf-8",
        )
        (middle_package / "native/middle.c").write_text(
            "int base_value(void);\n"
            "int middle_value(void) { return base_value() + 2; }\n",
            encoding="utf-8",
        )
        (middle_package / "src/mid.gti").write_text(
            'extern "C" {\n'
            "int middle_value();\n"
            "}\n\n"
            "namespace middle {\n"
            "int value() { return middle_value(); }\n"
            "}\n",
            encoding="utf-8",
        )

        chained_native_app = root / "chained-native-app"
        (chained_native_app / "src").mkdir(parents=True)
        (chained_native_app / "gti.toml").write_text(
            "manifest-version = 1\n\n"
            "[package]\n"
            'name = "chained-native-app"\n'
            'version = "0.1.0"\n\n'
            "[dependencies]\n"
            'middledep = { path = "../native-middle" }\n\n'
            "[targets.chained-native-app]\n"
            'kind = "executable"\n'
            'root = "src/main.gti"\n',
            encoding="utf-8",
        )
        (chained_native_app / "src/main.gti").write_text(
            "#include <middledep/mid>\n\n"
            "int main() {\n"
            "  return middle::value() == 42 ? 0 : 1;\n"
            "}\n",
            encoding="utf-8",
        )
        run([gti, "run"], cwd=chained_native_app)
        chained_metadata = json.loads(
            run([gti, "metadata"], cwd=chained_native_app).stdout
        )
        assert [
            group["package"]
            for group in chained_metadata["targets"][0]["outputs"][0][
                "native"
            ]["dependencyNative"]
        ] == ["middledep@0.1.0", "basedep@0.1.0"]

        # A failing dependency native compile reports the owning source and
        # stops before linking.
        (base_package / "native/base.c").write_text(
            "int base_value(void) { return missing; }\n", encoding="utf-8"
        )
        broken_dependency = run(
            [gti, "build"], expected=1, cwd=chained_native_app
        )
        assert "native C compiler failed" in broken_dependency.stderr
        assert "base.c" in broken_dependency.stderr
        (base_package / "native/base.c").write_text(
            "int base_value(void) { return 40; }\n", encoding="utf-8"
        )
        run([gti, "run"], cwd=chained_native_app)



if __name__ == "__main__":
    main()
