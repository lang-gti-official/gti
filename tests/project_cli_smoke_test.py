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
    with tempfile.TemporaryDirectory(prefix="gti-project-cli-test-") as directory:
        root = pathlib.Path(directory)

        new_project = root / "new-project"
        created = run([gti, "new", str(new_project)])
        assert "Created package 'new-project'" in created.stdout
        assert (new_project / "gti.toml").is_file()
        assert (new_project / "src/main.gti").is_file()
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

        generated = executable.parent / "intermediate/sample.gti.cpp"
        assert generated.is_file()
        direct_cpp = root / "direct.cpp"
        run([gti, str(source), "--emit-cpp", "-o", str(direct_cpp)])
        assert generated.read_bytes() == direct_cpp.read_bytes()

        run([gti, "build", "--no-keep-cpp"], cwd=project)
        assert not generated.exists()

        release = run(
            [gti, "build", "sample", "--release", "--verbose"], cwd=project
        )
        assert " -O3 " in release.stderr
        assert " -std=c++20 " in release.stderr
        assert "target sample [release," in release.stderr
        assert "Built sample [release," in release.stdout
        release_executable = executable_named(project / "build/gti/release", "sample")
        run([str(release_executable)])
        release_generated = (
            release_executable.parent / "intermediate/sample.gti.cpp"
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
                "\n[profiles.release]\noptimization = 3\n",
            ),
            encoding="utf-8",
        )
        metadata = run([gti, "metadata"], cwd=check_project)
        metadata_document = json.loads(metadata.stdout)
        assert metadata_document["schemaVersion"] == 4
        assert metadata_document["manifestVersion"] == 1
        assert metadata_document["package"]["name"] == "sample"
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
        }
        assert not (check_project / "build").exists()

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
        assert native_document["schemaVersion"] == 4
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
                "*/intermediate/*.native-0-native abi.o"
            )
        )
        assert len(native_objects) == 1
        native_object = native_objects[0]
        native_cpp_objects = list(
            (native_project / "build/gti/dev").glob(
                "*/intermediate/*.native-1-native support.o"
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


if __name__ == "__main__":
    main()
