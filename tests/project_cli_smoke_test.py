#!/usr/bin/env python3

import argparse
import hashlib
import json
import os
import pathlib
import shutil
import subprocess
import sys
import tempfile


DEPENDENCY_INJECTING_NATIVE_ENVIRONMENT = (
    "CPATH",
    "C_INCLUDE_PATH",
    "CPLUS_INCLUDE_PATH",
    "OBJC_INCLUDE_PATH",
    "LIBRARY_PATH",
    "COMPILER_PATH",
    "GCC_EXEC_PREFIX",
    "SDKROOT",
    "DEVELOPER_DIR",
    "LD_LIBRARY_PATH",
    "DYLD_LIBRARY_PATH",
    "DYLD_FRAMEWORK_PATH",
    "DYLD_FALLBACK_FRAMEWORK_PATH",
    "INCLUDE",
    "LIB",
)

GIT_REPOSITORY_ENVIRONMENT = (
    "GIT_DIR",
    "GIT_WORK_TREE",
    "GIT_COMMON_DIR",
    "GIT_INDEX_FILE",
    "GIT_OBJECT_DIRECTORY",
    "GIT_ALTERNATE_OBJECT_DIRECTORIES",
    "GIT_IMPLICIT_WORK_TREE",
)


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


def parse_arguments(arguments=None):
    parser = argparse.ArgumentParser(description="Exercise the GTI project CLI.")
    parser.add_argument("gti", help="path to the GTI executable")
    parser.add_argument(
        "--jobs", type=int, choices=range(1, 5), default=4,
        help="maximum concurrent target builds (default: 4; use 1 for a serial run)",
    )
    return parser.parse_args(arguments)


def main():
    arguments = parse_arguments()
    if arguments.jobs == 1:
        print("project_cli: serial resource limit; parallel scheduling parity "
              "requires the default four-job run", flush=True)

    requested_gti = pathlib.Path(arguments.gti)
    resolved_gti = (
        requested_gti.resolve()
        if requested_gti.exists()
        else shutil.which(arguments.gti)
    )
    if resolved_gti is None:
        raise AssertionError(f"could not resolve GTI executable: {arguments.gti}")
    gti = str(resolved_gti)
    git = shutil.which("git")
    if git is None:
        raise AssertionError("project CLI Git initialization requires git")
    # Cache checks need a deterministic environment. Some hosted Python
    # launchers inject SDK/include/library search paths before this script
    # starts; the driver correctly treats those mutable roots as a cache
    # bypass. Explicit environment-bypass coverage below adds CPATH back for
    # the command that owns that behavior.
    for variable in DEPENDENCY_INJECTING_NATIVE_ENVIRONMENT:
        os.environ.pop(variable, None)
    for variable in GIT_REPOSITORY_ENVIRONMENT:
        os.environ.pop(variable, None)
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

        git_init = root / "git-init"
        git_init.mkdir()
        initialized_git = run([gti, "init", str(git_init), "--git"])
        assert "Initialized package 'git-init'" in initialized_git.stdout
        assert "Initialized Git repository and wrote .gitignore" in (
            initialized_git.stdout
        )
        assert (git_init / ".git").is_dir()
        assert (git_init / ".gitignore").read_text(encoding="utf-8") == (
            "# GTI build output\n/build/\n"
        )
        top_level = run(
            [git, "-C", str(git_init), "rev-parse", "--show-toplevel"]
        )
        assert pathlib.Path(top_level.stdout.strip()).resolve() == git_init.resolve()
        ignored_build_file = git_init / "build" / "probe.txt"
        ignored_build_file.parent.mkdir()
        ignored_build_file.write_text("ignored\n", encoding="utf-8")
        run(
            [
                git,
                "-C",
                str(git_init),
                "check-ignore",
                "--quiet",
                "build/probe.txt",
            ]
        )

        new_with_git = run(
            [gti, "new", str(root / "new-with-git"), "--git"],
            expected=64,
        )
        assert "--git is supported only by gti init" in new_with_git.stderr
        assert not (root / "new-with-git").exists()

        duplicate_git = root / "duplicate-git"
        duplicate_git.mkdir()
        duplicate_git_option = run(
            [gti, "init", str(duplicate_git), "--git", "--git"],
            expected=64,
        )
        assert "--git may be specified only once" in duplicate_git_option.stderr
        assert not (duplicate_git / "gti.toml").exists()

        existing_ignore = root / "existing-ignore"
        existing_ignore.mkdir()
        (existing_ignore / ".gitignore").write_text(
            "user-owned\n", encoding="utf-8"
        )
        refused_ignore = run(
            [gti, "init", str(existing_ignore), "--git"], expected=64
        )
        assert "error[GTI-B1503]" in refused_ignore.stderr
        assert (existing_ignore / ".gitignore").read_text(
            encoding="utf-8"
        ) == "user-owned\n"
        assert not (existing_ignore / "gti.toml").exists()

        failed_git = root / "failed-git"
        failed_git.mkdir()
        missing_git_environment = os.environ.copy()
        missing_git_environment["GTI_GIT"] = str(root / "missing-git")
        git_failure = run(
            [gti, "init", str(failed_git), "--git"],
            expected=78,
            env=missing_git_environment,
        )
        assert "error[GTI-B1504]" in git_failure.stderr
        assert "Failed to initialize the Git repository" in git_failure.stderr
        assert not (failed_git / "gti.toml").exists()
        assert not (failed_git / "src").exists()
        assert not (failed_git / ".gitignore").exists()
        assert not (failed_git / ".git").exists()

        external_git = root / "external-git-state"
        external_git.mkdir()
        sentinel = external_git / "sentinel"
        sentinel.write_text("user-owned\n", encoding="utf-8")
        for variable in GIT_REPOSITORY_ENVIRONMENT:
            for value in (str(external_git), ""):
                protected_init = root / "environment-protected-init"
                protected_init.mkdir(exist_ok=True)
                environment = os.environ.copy()
                environment[variable] = value
                # Environment rejection must precede even executable discovery
                # failure, and must not initialize or rewrite either directory.
                environment["GTI_GIT"] = str(root / "missing-git")
                refused = run(
                    [gti, "init", str(protected_init), "--git"],
                    expected=64,
                    env=environment,
                )
                assert "error[GTI-B1500]" in refused.stderr
                assert variable in refused.stderr
                assert "unset that environment variable" in refused.stderr
                assert list(protected_init.iterdir()) == []
                assert list(external_git.iterdir()) == [sentinel]
                assert sentinel.read_text(encoding="utf-8") == "user-owned\n"

        # An existing Git directory is not scaffold-owned, even on failure.
        protected_repository = root / "protected-repository"
        metadata = protected_repository / ".git"
        metadata.mkdir(parents=True)
        git_sentinel = metadata / "sentinel"
        git_sentinel.write_text("repository-owned\n", encoding="utf-8")
        refused_repository = run(
            [gti, "init", str(protected_repository), "--git"], expected=64
        )
        assert "error[GTI-B1503]" in refused_repository.stderr
        assert git_sentinel.read_text(encoding="utf-8") == "repository-owned\n"
        assert not (protected_repository / "gti.toml").exists()

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

        warning_project = root / "warning-project"
        warning_source = warning_project / "src/main.gti"
        warning_source.parent.mkdir(parents=True)
        warning_source.write_text(
            "int main() { mut int result = 0; return result; }\n",
            encoding="utf-8",
        )
        warning_manifest = warning_project / "gti.toml"
        warning_manifest.write_text(
            "manifest-version = 1\n\n"
            "[package]\n"
            'name = "warning_project"\n'
            'version = "0.1.0"\n\n'
            "[targets.warning_project]\n"
            'kind = "executable"\n'
            'root = "src/main.gti"\n\n'
            "[profiles.dev]\n"
            'disabled-warnings = ["unused-mut"]\n',
            encoding="utf-8",
        )
        warning_build = run([gti, "build", "--no-cache"], cwd=warning_project)
        assert "GTI-S2096" not in warning_build.stderr
        assert (
            warning_build.stdout.splitlines()[-1]
            == "Diagnostics: 0 errors, 0 warnings"
        )
        warning_manifest.write_text(
            warning_manifest.read_text(encoding="utf-8").replace(
                'disabled-warnings = ["unused-mut"]\n', ""
            ),
            encoding="utf-8",
        )
        warning_check = run([gti, "check"], cwd=warning_project)
        assert "warning[GTI-S2096]" in warning_check.stderr
        assert (
            warning_check.stdout.splitlines()[-1]
            == "Diagnostics: 0 errors, 1 warning"
        )
        warning_check_suppressed = run(
            [gti, "check", "-Wno-unused-mut"], cwd=warning_project
        )
        assert "GTI-S2096" not in warning_check_suppressed.stderr
        assert (
            warning_check_suppressed.stdout.splitlines()[-1]
            == "Diagnostics: 0 errors, 0 warnings"
        )

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

        configuration_project = root / "configuration-project"
        configuration_source = configuration_project / "src/main.gti"
        configuration_source.parent.mkdir(parents=True)
        configuration_source.write_text(
            "#if defined(MANIFEST_FLAG) && defined(CLI_FLAG)\n"
            "int main() { return 0; }\n"
            "#else\n"
            "int main() { return 1; }\n"
            "#endif\n",
            encoding="utf-8",
        )
        (configuration_project / "gti.toml").write_text(
            manifest(
                "[targets.configuration]\n"
                'kind = "executable"\n'
                'root = "src/main.gti"\n'
            )
            + '\n[build]\ndefines = ["MANIFEST_FLAG"]\n',
            encoding="utf-8",
        )
        run([gti, "build", "-D", "CLI_FLAG"], cwd=configuration_project)
        configuration_executable = executable_named(
            configuration_project / "build/gti/dev", "configuration"
        )
        run([str(configuration_executable)])
        configuration_metadata = json.loads(
            run([gti, "metadata"], cwd=configuration_project).stdout
        )
        assert configuration_metadata["schemaVersion"] == 13
        assert configuration_metadata["targets"][0]["outputs"][0][
            "defines"
        ] == ["MANIFEST_FLAG"]

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

        # --no-build resolves the selected managed output but deliberately does
        # not parse changed source, regenerate bindings, build dependencies, or
        # consult the cache before launching it.
        source.write_text("this is deliberately invalid GTI\n", encoding="utf-8")
        no_build_run = run(
            [gti, "run", "--no-build", "--", "preserved argument"], cwd=nested
        )
        assert no_build_run.stdout == ""
        assert "Running existing " in no_build_run.stderr
        assert "(--no-build)" in no_build_run.stderr
        assert "Built sample" not in no_build_run.stderr
        source.write_text("int main() { return 0; }\n", encoding="utf-8")

        saved_executable = executable.with_name(executable.name + ".saved")
        executable.rename(saved_executable)
        missing_no_build = run(
            [gti, "run", "--no-build"], expected=74, cwd=nested
        )
        assert "managed executable does not exist" in missing_no_build.stderr
        assert "build target 'sample' before using --no-build" in (
            missing_no_build.stderr
        )
        saved_executable.rename(executable)
        invalid_no_build_command = run(
            [gti, "build", "--no-build"], expected=64, cwd=nested
        )
        assert "supported only by gti run" in invalid_no_build_command.stderr
        conflicting_no_build = run(
            [gti, "run", "--no-build", "--no-cache"],
            expected=64,
            cwd=nested,
        )
        assert "--no-build cannot be combined" in conflicting_no_build.stderr

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
        assert "gti: cache hit " in cached_build.stderr, cached_build.stderr
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
                'execution-profile = "concurrent"\n'
                'disabled-warnings = ["unused-mut"]\n',
            ),
            encoding="utf-8",
        )
        metadata = run([gti, "metadata"], cwd=check_project)
        metadata_document = json.loads(metadata.stdout)
        assert metadata_document["schemaVersion"] == 13
        assert metadata_document["manifestVersion"] == 1
        assert metadata_document["package"]["name"] == "sample"
        assert metadata_document["profiles"] == [
            {
                "name": "dev",
                "optimization": 0,
                "cppStandard": "c++23",
                "executionProfile": "single-threaded",
                "disabledWarnings": [],
                "keepCpp": False,
                "debugInfo": False,
            },
            {
                "name": "release",
                "optimization": 3,
                "cppStandard": "c++23",
                "executionProfile": "concurrent",
                "disabledWarnings": ["unused-mut"],
                "keepCpp": False,
                "debugInfo": False,
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
        assert test_metadata["schemaVersion"] == 13
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
        assert native_document["schemaVersion"] == 13
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
        assert workspace_metadata["schemaVersion"] == 13
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
            / "tests/fixtures/project_cli_layer_stack"
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

        # --- Name-resolved libraries and library search directories ---
        # A declared -L directory precedes every implicit toolchain directory
        # on the link line, so the first declared directory holding exactly
        # one modeled candidate is the file the linker selects. That file's
        # content, and the full tree identity of every declared search
        # directory, join the key.
        cache_ar = shutil.which("ar")
        if cache_ar is None:
            raise AssertionError("native cache tests require ar on PATH")
        resolved_archive = cache_native / "native/lib/libextra.a"
        # Archives are staged outside the declared search directory so
        # installing one is a byte-exact copy: `ar` records member timestamps,
        # so rebuilding would change content that the key correctly tracks.
        archive_stage = cache_native / "native/archives"
        archive_stage.mkdir(parents=True, exist_ok=True)

        def stage_resolved_archive(bias):
            variant = archive_stage / f"bias{bias}.c"
            variant.write_text(
                "int native_add(int left, int right) {\n"
                f"  return left + right + {bias};\n"
                "}\n",
                encoding="utf-8",
            )
            variant_object = archive_stage / f"bias{bias}.o"
            run([cache_cc, "-c", str(variant), "-o", str(variant_object)])
            archive = archive_stage / f"libextra{bias}.a"
            if archive.exists():
                archive.unlink()
            run([cache_ar, "rcs", str(archive), str(variant_object)])
            return archive

        unbiased_archive = stage_resolved_archive(0)
        biased_archive = stage_resolved_archive(1)

        def install_resolved_archive(archive):
            shutil.copyfile(archive, resolved_archive)

        install_resolved_archive(unbiased_archive)
        cache_native_manifest(
            [
                'include-dirs = ["native/include"]',
                'library-dirs = ["native/lib"]',
                'libraries = ["extra"]',
            ]
        )
        resolved_first = cache_native_build()
        assert "gti: cache miss " in resolved_first.stderr
        resolved_key = cache_key(resolved_first)
        resolved_second = cache_native_build()
        assert "gti: cache hit " in resolved_second.stderr
        assert cache_key(resolved_second) == resolved_key
        run([gti, "run"], cwd=cache_native)

        # Staleness safety: the resolved archive's content is part of the key,
        # so replacing it forces a rebuild that observes the new behavior.
        install_resolved_archive(biased_archive)
        restaled = cache_native_build()
        assert "gti: cache miss " in restaled.stderr
        assert cache_key(restaled) != resolved_key
        run([gti, "run"], expected=1, cwd=cache_native)
        install_resolved_archive(unbiased_archive)
        rerestored = cache_native_build()
        assert "gti: cache hit " in rerestored.stderr
        assert cache_key(rerestored) == resolved_key
        run([gti, "run"], cwd=cache_native)

        # A name with no candidate in any declared directory would resolve
        # through the linker's implicit system directories, which the driver
        # does not model.
        cache_native_manifest(
            [
                'include-dirs = ["native/include"]',
                'library-dirs = ["native/lib"]',
                'libraries = ["extra", "m"]',
            ]
        )
        unresolved_bypass = cache_native_build()
        assert "gti: cache bypassed" in unresolved_bypass.stderr
        assert (
            "native library '-lm' does not resolve inside the declared "
            "native library search directories"
        ) in unresolved_bypass.stderr

        # Two modeled candidates in one directory: which file the linker
        # prefers is a guess, so resolution refuses rather than guessing.
        resolved_dynamic = cache_native / (
            "native/lib/libextra."
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
                str(resolved_dynamic),
            ]
        )
        cache_native_manifest(
            [
                'include-dirs = ["native/include"]',
                'library-dirs = ["native/lib"]',
                'libraries = ["extra"]',
            ]
        )
        ambiguous_bypass = cache_native_build()
        assert "gti: cache bypassed" in ambiguous_bypass.stderr
        assert "resolves ambiguously" in ambiguous_bypass.stderr

        # An unambiguous resolution to a file whose bytes do not determine its
        # contribution to the link still bypasses.
        resolved_archive.unlink()
        resolved_dynamic_bypass = cache_native_build()
        assert "gti: cache bypassed" in resolved_dynamic_bypass.stderr
        assert (
            "not a content-complete archive or object"
            in resolved_dynamic_bypass.stderr
        )

        # A spelling the driver does not model must make resolution ambiguous
        # rather than invisible.
        resolved_dynamic.unlink()
        install_resolved_archive(unbiased_archive)
        unmodelled = cache_native / "native/lib/libextra.la"
        unmodelled.write_text("# libtool archive\n", encoding="utf-8")
        unmodelled_bypass = cache_native_build()
        assert "gti: cache bypassed" in unmodelled_bypass.stderr
        assert "resolves ambiguously" in unmodelled_bypass.stderr
        unmodelled.unlink()

        if sys.platform == "darwin":
            cache_native_manifest(
                [
                    'include-dirs = ["native/include"]',
                    'library-dirs = ["native/lib"]',
                    'libraries = ["extra"]',
                    'frameworks = ["CoreFoundation"]',
                ]
            )
            framework_miss = cache_native_build()
            assert "gti: cache miss " in framework_miss.stderr
            assert "gti: cache bypassed" not in framework_miss.stderr
            framework_hit = cache_native_build()
            assert "gti: cache hit " in framework_hit.stderr
            assert not any(
                line.startswith("+ ")
                for line in framework_hit.stderr.splitlines()
            )

        resolved_archive.unlink()

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

        # Without any declared search directory a named library can only come
        # from the linker's implicit directories.
        cache_native_manifest(
            [
                'include-dirs = ["native/include"]',
                'c-sources = ["native/support.c"]',
                'libraries = ["m"]',
            ]
        )
        named_bypass = cache_native_build()
        assert "gti: cache bypassed" in named_bypass.stderr
        assert (
            "native library '-lm' does not resolve inside the declared "
            "native library search directories"
        ) in named_bypass.stderr

        # A declared library search directory joins as a full tree identity,
        # so it participates instead of bypassing, and any file added below it
        # changes the key.
        cache_native_manifest(
            [
                'include-dirs = ["native/include"]',
                'c-sources = ["native/support.c"]',
                'library-dirs = ["native/lib"]',
            ]
        )
        library_dir_first = cache_native_build()
        assert "gti: cache miss " in library_dir_first.stderr
        library_dir_key = cache_key(library_dir_first)
        library_dir_second = cache_native_build()
        assert "gti: cache hit " in library_dir_second.stderr
        assert cache_key(library_dir_second) == library_dir_key
        # Any file appearing below a declared search directory changes the
        # key, which is what makes the directory exact: a file dropped there
        # could otherwise shadow a library the toolchain resolves implicitly.
        added_library_file = cache_native / "native/lib/added.txt"
        added_library_file.write_text("added", encoding="utf-8")
        library_dir_shadowed = cache_native_build()
        assert cache_key(library_dir_shadowed) != library_dir_key
        added_library_file.unlink()
        library_dir_restored = cache_native_build()
        assert "gti: cache hit " in library_dir_restored.stderr
        assert cache_key(library_dir_restored) == library_dir_key

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

        # --- Dependency native groups in the whole-program cache ---
        # A dependency composes only its own include directories, validated
        # exact -D/-U macro definitions, and declared sources, so the same
        # preprocessor discovery that makes the selected package's native
        # sources exact applies unchanged to a group's.
        dep_native = root / "cache-dependency-native"
        dep_app = dep_native / "packages/app"
        dep_lib = dep_native / "packages/nativedep"
        (dep_native / "src").mkdir(parents=True)
        (dep_app / "src").mkdir(parents=True)
        (dep_lib / "src").mkdir(parents=True)
        (dep_lib / "native/include").mkdir(parents=True)
        (dep_native / "src/main.gti").write_text(
            "int main() { return 0; }\n", encoding="utf-8"
        )
        (dep_native / "gti.toml").write_text(
            "manifest-version = 1\n\n"
            "[package]\n"
            'name = "cache_dependency_root"\n'
            'version = "0.1.0"\n\n'
            "[targets.root]\n"
            'kind = "executable"\n'
            'root = "src/main.gti"\n\n'
            "[workspace]\n"
            'members = ["packages/nativedep", "packages/app"]\n',
            encoding="utf-8",
        )
        (dep_app / "gti.toml").write_text(
            "manifest-version = 1\n\n"
            "[package]\n"
            'name = "app"\n'
            'version = "1.0.0"\n\n'
            "[dependencies]\n"
            'nativedep = { path = "../nativedep" }\n\n'
            "[targets.app]\n"
            'kind = "executable"\n'
            'root = "src/main.gti"\n',
            encoding="utf-8",
        )
        (dep_app / "src/main.gti").write_text(
            "#include <nativedep/api>\n"
            "int main() { return nativedep::scaled(21) == 42 ? 0 : 1; }\n",
            encoding="utf-8",
        )
        (dep_lib / "gti.toml").write_text(
            "manifest-version = 1\n\n"
            "[package]\n"
            'name = "nativedep"\n'
            'version = "2.0.0"\n\n'
            "[package.native]\n"
            'include-dirs = ["native/include"]\n'
            'c-sources = ["native/dep.c"]\n',
            encoding="utf-8",
        )
        dep_header = dep_lib / "native/include/dep.h"
        dep_header.write_text(
            "#define DEP_FACTOR 2\n" "int dep_scale(int value);\n",
            encoding="utf-8",
        )
        (dep_lib / "native/dep.c").write_text(
            '#include "dep.h"\n'
            "int dep_scale(int value) { return value * DEP_FACTOR; }\n",
            encoding="utf-8",
        )
        (dep_lib / "src/api.gti").write_text(
            'extern "C" {\n'
            "int dep_scale(int value);\n"
            "}\n\n"
            "namespace nativedep {\n"
            "int scaled(int value) { return dep_scale(value); }\n"
            "}\n",
            encoding="utf-8",
        )

        def dep_native_build(expected=0):
            return run(
                [
                    gti,
                    "build",
                    "--package",
                    "app",
                    "--verbose",
                    "--cc",
                    cache_cc,
                    "--cxx",
                    cache_cxx,
                ],
                expected=expected,
                cwd=dep_native,
            )

        dep_first = dep_native_build()
        assert "gti: cache miss " in dep_first.stderr
        dep_key = cache_key(dep_first)
        dep_second = dep_native_build()
        assert "gti: cache hit " in dep_second.stderr
        assert cache_key(dep_second) == dep_key
        assert not any(
            line.startswith("+ ") for line in dep_second.stderr.splitlines()
        )
        run([gti, "run", "--package", "app"], cwd=dep_native)

        # A header discovered only through the dependency's own preprocessor
        # probe is part of the key.
        dep_header.write_text(
            "#define DEP_FACTOR 3\n" "int dep_scale(int value);\n",
            encoding="utf-8",
        )
        dep_revised = dep_native_build()
        assert "gti: cache miss " in dep_revised.stderr
        assert cache_key(dep_revised) != dep_key
        run([gti, "run", "--package", "app"], expected=1, cwd=dep_native)
        dep_header.write_text(
            "#define DEP_FACTOR 2\n" "int dep_scale(int value);\n",
            encoding="utf-8",
        )
        dep_restored = dep_native_build()
        assert "gti: cache hit " in dep_restored.stderr
        assert cache_key(dep_restored) == dep_key

        # A dependency's own time-and-date macro use still bypasses.
        (dep_lib / "native/dep.c").write_text(
            '#include "dep.h"\n'
            "int dep_scale(int value) {\n"
            "  return value * DEP_FACTOR + (__TIME__[0] == 0);\n"
            "}\n",
            encoding="utf-8",
        )
        dep_time_macro = dep_native_build()
        assert "gti: cache bypassed" in dep_time_macro.stderr
        assert "time-and-date preprocessor macros" in dep_time_macro.stderr

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
            [gti, "build", "--all", "--jobs", str(arguments.jobs), "--no-cache"],
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
        # Keep default-job CLI coverage in the ordinary run, but do not let
        # hardware-concurrency defaults escape an explicit local resource cap.
        cached_jobs = [] if arguments.jobs == 4 else ["--jobs", str(arguments.jobs)]
        cached_all = run([gti, "build", "--all", *cached_jobs], cwd=multi_project)
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
            [gti, "build", "--all", "--jobs", str(arguments.jobs), "--no-cache"],
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

        # Option surface errors stay focused. --jobs also controls dependency
        # build scripts, so it no longer requires --all.
        all_with_target = run(
            [gti, "build", "--all", "alpha"], expected=64, cwd=multi_project
        )
        assert "--all cannot be combined with a target selection" in (
            all_with_target.stderr
        )
        jobs_without_all = run(
            [gti, "build", "--jobs", "2"], expected=65, cwd=multi_project
        )
        assert "declares multiple targets" in jobs_without_all.stderr
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

        # --- Consumer-approved dependency build scripts ---
        scripted = root / "scripted-native"
        (scripted / "src").mkdir(parents=True)
        (scripted / "gti.toml").write_text(
            "manifest-version = 1\n\n"
            "[package]\n"
            'name = "scripted-native"\n'
            'version = "0.1.0"\n\n'
            "[package.build-script]\n"
            'source = "build.gti"\n'
            'inputs = []\n',
            encoding="utf-8",
        )
        (scripted / "src/scripted.gti").write_text(
            "namespace scripted {\n"
            'extern "C" {\n'
            "int scripted_value();\n"
            "}\n"
            "}\n",
            encoding="utf-8",
        )
        (scripted / "build.gti").write_text(
            "#include <std/build>\n"
            "#include <std/cstdio>\n"
            "#include <std/string>\n"
            "#include <std/vector>\n\n"
            "int main(int argc, std::vector<std::string> argv) {\n"
            "  if (argc != 11) { return 64; }\n"
            "  mut std::build::context context = "
            "std::build::context_from_args(argv);\n"
            "  mut std::string source = "
            "std::string(context.output_dir().view());\n"
            "  source.append(\"/scripted.c\");\n"
            "  mut auto file = std::fopen(source.view(), "
            "std::file_mode::write);\n"
            "  if (!file) { return 74; }\n"
            "  auto written = file.value()->write(\n"
            "      \"int scripted_value(void) { return 42; }\\n\");\n"
            "  if (!written) { return 74; }\n"
            "  auto closed = file.value()->close();\n"
            "  if (!closed) { return 74; }\n"
            "  mut std::build::outputs outputs = "
            "std::build::outputs();\n"
            "  outputs.c_source(\"scripted.c\");\n"
            "  return outputs.publish(context);\n"
            "}\n",
            encoding="utf-8",
        )
        scripted_app = root / "scripted-app"
        (scripted_app / "src").mkdir(parents=True)
        scripted_manifest = (
            "manifest-version = 1\n\n"
            "[package]\n"
            'name = "scripted-app"\n'
            'version = "0.1.0"\n\n'
            "[dependencies]\n"
            'scripted = { path = "../scripted-native" }\n\n'
            "[build]\n"
            'allow-build-scripts = ["scripted-native@0.1.0"]\n\n'
            "[targets.scripted]\n"
            'kind = "test"\n'
            'root = "src/main.gti"\n'
        )
        (scripted_app / "gti.toml").write_text(
            scripted_manifest, encoding="utf-8"
        )
        (scripted_app / "src/main.gti").write_text(
            "#include <scripted/scripted>\n\n"
            "int main() {\n"
            "  unsafe { return scripted::scripted_value() == 42 ? 0 : 1; }\n"
            "}\n",
            encoding="utf-8",
        )
        scripted_metadata = json.loads(
            run([gti, "metadata"], cwd=scripted_app).stdout
        )
        assert scripted_metadata["schemaVersion"] == 13
        assert scripted_metadata["build"]["allowBuildScripts"] == [
            "scripted-native@0.1.0"
        ]
        dependency_metadata = next(
            package
            for package in scripted_metadata["workspace"]["packages"]
            if package["name"] == "scripted-native"
        )
        assert dependency_metadata["buildScript"]["command"] == []
        assert dependency_metadata["buildScript"]["source"] == "build.gti"
        script_plan = scripted_metadata["targets"][0]["outputs"][0][
            "buildScripts"
        ][0]
        assert script_plan["package"] == "scripted-native@0.1.0"
        assert script_plan["source"].endswith("/build.gti")
        assert not (scripted_app / "build").exists()
        built_scripted = run(
            [gti, "test", "--jobs", "2"], cwd=scripted_app
        )
        assert "Built native dependency scripted-native@0.1.0" in (
            built_scripted.stderr
        )
        reused_scripted = run(
            [gti, "test", "--jobs", "2"], cwd=scripted_app
        )
        assert "Reused native dependency scripted-native@0.1.0" in (
            reused_scripted.stderr
        )
        (scripted_app / "gti.toml").write_text(
            scripted_manifest.replace(
                'allow-build-scripts = ["scripted-native@0.1.0"]',
                "allow-build-scripts = []",
            ),
            encoding="utf-8",
        )
        unapproved_script = run([gti, "check"], expected=65, cwd=scripted_app)
        assert "error[GTI-B2002]" in unapproved_script.stderr


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
        # Dependency native groups reach the same preprocessor discovery as
        # the selected package's own sources, so a plan carrying them joins
        # cache identity instead of bypassing.
        assert "gti: cache miss " in composed_build.stderr
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
        assert composed_metadata["schemaVersion"] == 13
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


        # --- A git dependency that vendors a C library ---
        # The portability claim this proves: a consumer clones nothing by hand,
        # installs no system library, and still builds and runs. The dependency
        # carries the C source and header, generates its own GTI bindings for
        # them, and the consumer reaches the library only through the
        # dependency's GTI wrapper -- bindgen aliases do not leak to dependents.
        vendored_upstream = root / "vendored-c-upstream"
        (vendored_upstream / "native").mkdir(parents=True)
        (vendored_upstream / "src").mkdir(parents=True)
        (vendored_upstream / "gti.toml").write_text(
            "manifest-version = 1\n\n"
            "[package]\n"
            'name = "checksum"\n'
            'version = "0.1.0"\n\n'
            "[package.native]\n"
            'include-dirs = ["native"]\n'
            'c-sources = ["native/checksum.c"]\n\n'
            "[bindgen.checksum_c]\n"
            'header = "native/checksum.h"\n',
            encoding="utf-8",
        )
        (vendored_upstream / "native/checksum.h").write_text(
            "#ifndef CHECKSUM_H\n"
            "#define CHECKSUM_H\n"
            "#include <stdint.h>\n"
            "#define CHECKSUM_SEED 17\n"
            "int32_t checksum_of(const uint8_t *bytes, int32_t count);\n"
            "#endif\n",
            encoding="utf-8",
        )
        (vendored_upstream / "native/checksum.c").write_text(
            '#include "checksum.h"\n'
            "int32_t checksum_of(const uint8_t *bytes, int32_t count) {\n"
            "  int32_t total = CHECKSUM_SEED;\n"
            "  for (int32_t index = 0; index < count; index++) {\n"
            "    total = total + (int32_t)bytes[index];\n"
            "  }\n"
            "  return total;\n"
            "}\n",
            encoding="utf-8",
        )
        (vendored_upstream / "src/lib.gti").write_text(
            "#include <checksum_c/bindings>\n"
            "\n"
            "// Wrapped so dependents never touch the raw C ABI.\n"
            "int32_t checksum_of_bytes(uint8_t* bytes, int32_t count) {\n"
            "  unsafe {\n"
            "    return checksum_of(bytes, count);\n"
            "  }\n"
            "}\n"
            "\n"
            "int32_t checksum_seed() {\n"
            "  return CHECKSUM_SEED;\n"
            "}\n",
            encoding="utf-8",
        )
        run([git, "init", "-q", str(vendored_upstream)])
        git_in(vendored_upstream, "add", "-A")
        git_in(vendored_upstream, "commit", "-qm", "vendored checksum library")
        vendored_revision = git_in(
            vendored_upstream, "rev-parse", "HEAD"
        ).stdout.strip()

        vendored_app = root / "vendored-c-app"
        (vendored_app / "src").mkdir(parents=True)
        (vendored_app / "gti.toml").write_text(
            "manifest-version = 1\n\n"
            "[package]\n"
            'name = "vendored-app"\n'
            'version = "0.1.0"\n\n'
            "[dependencies]\n"
            f'checksum = {{ git = "{vendored_upstream}", rev = '
            f'"{vendored_revision}" }}\n\n'
            "[targets.vendored-app]\n"
            'kind = "executable"\n'
            'root = "src/main.gti"\n',
            encoding="utf-8",
        )
        (vendored_app / "src/main.gti").write_text(
            "#include <checksum/lib>\n"
            "\n"
            "int main() {\n"
            "  mut uint8_t data[3] = {uint8_t(1), uint8_t(2), uint8_t(3)};\n"
            "  unsafe {\n"
            "    int32_t total = checksum_of_bytes(&data[0], 3);\n"
            "    if (total != 23) {\n"
            "      return 1;\n"
            "    }\n"
            "  }\n"
            "  return checksum_seed() == 17 ? 0 : 1;\n"
            "}\n",
            encoding="utf-8",
        )
        run([gti, "fetch"], cwd=vendored_app)
        vendored_build = run([gti, "build"], cwd=vendored_app)
        # The dependency's own bindgen import generated inside its checkout.
        assert "Generated bindings checksum_c" in vendored_build.stdout
        # `run` exits 0 only if the vendored C actually computed 17+1+2+3.
        run([gti, "run"], cwd=vendored_app)
        # The consumer must not reach the dependency's bindgen alias.
        (vendored_app / "src/main.gti").write_text(
            "#include <checksum_c/bindings>\n"
            "\n"
            "int main() {\n"
            "  return 0;\n"
            "}\n",
            encoding="utf-8",
        )
        leaked = run([gti, "build"], expected=65, cwd=vendored_app)
        assert "checksum_c" in leaked.stderr

        # --- Manifest-declared bindgen imports (BUILD-01) ---
        clang = shutil.which("clang")
        if clang is None:
            raise AssertionError("project bindgen test requires clang on PATH")
        bindgen_project = root / "bindgen-project"
        (bindgen_project / "src").mkdir(parents=True)
        (bindgen_project / "native").mkdir()
        (bindgen_project / "gti.toml").write_text(
            "manifest-version = 1\n\n"
            "[package]\n"
            'name = "bindgen-project"\n'
            'version = "0.1.0"\n\n'
            "[package.native]\n"
            'c-sources = ["native/counter.c"]\n'
            'c-compile-args = ["-DCOUNTER_STEP=2"]\n\n'
            "[targets.bindgen-project]\n"
            'kind = "executable"\n'
            'root = "src/main.gti"\n\n'
            "[bindgen.counter]\n"
            'header = "native/counter.h"\n'
            'defines = ["COUNTER_STEP=2"]\n',
            encoding="utf-8",
        )
        (bindgen_project / "native/counter.h").write_text(
            "#ifndef COUNTER_H\n"
            "#define COUNTER_H\n"
            "#ifndef COUNTER_STEP\n"
            "#define COUNTER_STEP 1\n"
            "#endif\n"
            "#define COUNTER_LIMIT (40 + COUNTER_STEP)\n"
            "typedef struct Counter Counter;\n"
            "Counter *counter_create(int start);\n"
            "void counter_step(Counter *counter, void (*observer)(int value));\n"
            "int counter_value(const Counter *counter);\n"
            "void counter_destroy(Counter *counter);\n"
            "#endif\n",
            encoding="utf-8",
        )
        (bindgen_project / "native/counter.c").write_text(
            '#include "counter.h"\n'
            "#include <stdlib.h>\n"
            "struct Counter { int value; };\n"
            "Counter *counter_create(int start) {\n"
            "  Counter *counter = malloc(sizeof(Counter));\n"
            "  counter->value = start;\n"
            "  return counter;\n"
            "}\n"
            "void counter_step(Counter *counter, void (*observer)(int value)) {\n"
            "  counter->value += COUNTER_STEP;\n"
            "  if (observer) { observer(counter->value); }\n"
            "}\n"
            "int counter_value(const Counter *counter) {\n"
            "  return counter->value;\n"
            "}\n"
            "void counter_destroy(Counter *counter) { free(counter); }\n",
            encoding="utf-8",
        )
        (bindgen_project / "src/main.gti").write_text(
            "#include <counter/bindings>\n"
            "\n"
            "int main() {\n"
            "  unsafe {\n"
            "    mut Counter* counter = counter_create(40);\n"
            "    counter_step(counter, nullptr);\n"
            "    int32_t value = counter_value(counter);\n"
            "    counter_destroy(counter);\n"
            "    if (value != COUNTER_LIMIT) {\n"
            "      return 1;\n"
            "    }\n"
            "  }\n"
            "  return 0;\n"
            "}\n",
            encoding="utf-8",
        )
        # The imported macro constant, the opaque handle, the synthesized
        # callback alias, and the C implementation cross one real build+run.
        bindgen_build = run([gti, "build"], cwd=bindgen_project)
        assert "Generated bindings counter (" in bindgen_build.stdout
        generated_bindings = (
            bindgen_project / "build/gti/bindgen/bindgen-project/counter/bindings.gti"
        )
        assert generated_bindings.is_file()
        assert "COUNTER_LIMIT" in generated_bindings.read_text(encoding="utf-8")
        run([gti, "run"], cwd=bindgen_project)
        # An unchanged header reuses the published bindings silently.
        bindgen_rebuild = run([gti, "build"], cwd=bindgen_project)
        assert "Generated bindings" not in bindgen_rebuild.stdout
        # A header edit regenerates through the reuse key.
        with (bindgen_project / "native/counter.h").open("a") as header:
            header.write("int counter_reset(Counter *counter);\n")
        bindgen_regenerate = run([gti, "check"], cwd=bindgen_project)
        assert "Generated bindings counter (" in bindgen_regenerate.stdout
        # A header escaping the package is refused at manifest validation.
        (bindgen_project / "gti.toml").write_text(
            (bindgen_project / "gti.toml")
            .read_text(encoding="utf-8")
            .replace('header = "native/counter.h"', 'header = "../outside.h"'),
            encoding="utf-8",
        )
        escaped = run([gti, "build"], expected=65, cwd=bindgen_project)
        assert "GTI-B1104" in escaped.stderr

        # --- Opt-in import policy (C-07) ---
        policy_project = root / "bindgen-policy"
        (policy_project / "src").mkdir(parents=True)
        (policy_project / "native").mkdir()
        (policy_project / "gti.toml").write_text(
            "manifest-version = 1\n\n"
            "[package]\n"
            'name = "bindgen-policy"\n'
            'version = "0.1.0"\n\n'
            "[package.native]\n"
            'c-sources = ["native/points.c"]\n\n'
            "[targets.bindgen-policy]\n"
            'kind = "executable"\n'
            'root = "src/main.gti"\n\n'
            "[bindgen.points]\n"
            'header = "native/points.h"\n'
            'deny = ["points_internal_*"]\n'
            'rename = { points__store = "PointStore" }\n'
            'array-returns = [{ function = "points_all", count = "count" }]\n',
            encoding="utf-8",
        )
        (policy_project / "native/points.h").write_text(
            "#ifndef POINTS_H\n"
            "#define POINTS_H\n"
            '#define POINTS_LABEL "demo"\n'
            "#define POINTS_SCALE 1.5\n"
            "typedef struct Point { int x; int y; } Point;\n"
            "struct points__store { int used; };\n"
            "struct points__store *points_open(void);\n"
            "void points_close(struct points__store *store);\n"
            "int points_add(struct points__store *store, int x, int y);\n"
            "const Point *points_all(struct points__store *store,"
            " unsigned int *count);\n"
            "int points_internal_debug(int value);\n"
            "#endif\n",
            encoding="utf-8",
        )
        (policy_project / "native/points.c").write_text(
            '#include "points.h"\n'
            "#include <stdlib.h>\n"
            "static Point storage[8];\n"
            "struct points__store *points_open(void) {\n"
            "  struct points__store *store ="
            " malloc(sizeof(struct points__store));\n"
            "  store->used = 0;\n"
            "  return store;\n"
            "}\n"
            "void points_close(struct points__store *store) { free(store); }\n"
            "int points_add(struct points__store *store, int x, int y) {\n"
            "  storage[store->used].x = x;\n"
            "  storage[store->used].y = y;\n"
            "  store->used += 1;\n"
            "  return store->used;\n"
            "}\n"
            "const Point *points_all(struct points__store *store,"
            " unsigned int *count) {\n"
            "  *count = (unsigned int)store->used;\n"
            "  return storage;\n"
            "}\n"
            "int points_internal_debug(int value) { return value; }\n",
            encoding="utf-8",
        )
        (policy_project / "src/main.gti").write_text(
            "#include <points/bindings>\n"
            "\n"
            "int main() {\n"
            "  unsafe {\n"
            "    mut PointStore* store = points_open();\n"
            "    [[discard]] points_add(store, 3, 4);\n"
            "    [[discard]] points_add(store, 5, 6);\n"
            "    mut uint32_t count = 0;\n"
            "    const Point* all = points_all(store, &count);\n"
            "    int32_t total = count == 2 ? all[1].x + all[1].y : 0;\n"
            "    points_close(store);\n"
            "    if (total != 11) {\n"
            "      return 1;\n"
            "    }\n"
            "  }\n"
            "  return 0;\n"
            "}\n",
            encoding="utf-8",
        )
        # The rename makes a reserved-spelling record usable, the declared
        # array return emits [[c_array]], and the denied name stays out.
        run([gti, "build"], cwd=policy_project)
        policy_bindings = (
            policy_project
            / "build/gti/bindgen/bindgen-policy/points/bindings.gti"
        ).read_text(encoding="utf-8")
        assert "struct PointStore {" in policy_bindings
        assert "[[c_array(count)]] const Point* points_all(" in policy_bindings
        assert "points_internal_debug" not in policy_bindings
        assert 'constexpr std::string_view POINTS_LABEL = "demo";' in (
            policy_bindings
        )
        assert "constexpr double POINTS_SCALE = 1.5;" in policy_bindings
        run([gti, "run"], cwd=policy_project)

        # Cross compilation. `--target` selects the build target for project
        # commands; a target that is not the build host must resolve a
        # declared [toolchains.<triple>] table rather than silently reuse
        # host tools.
        cross_triple = (
            "x86_64-unknown-linux-gnu"
            if host_target_os() != "linux"
            else "arm64-apple-macos"
        )
        cross_project = root / "cross"
        (cross_project / "src").mkdir(parents=True)
        (cross_project / "sysroot").mkdir(parents=True)
        (cross_project / "src/main.gti").write_text(
            "int main() { return 0; }\n", encoding="utf-8"
        )
        host_only_manifest = manifest(
            '[targets.cross]\nkind = "executable"\nroot = "src/main.gti"\n'
        )
        (cross_project / "gti.toml").write_text(
            host_only_manifest, encoding="utf-8"
        )

        # The host build of an unchanged manifest keeps working.
        run([gti, "build"], cwd=cross_project)

        # Fail closed: no declared toolchain for a non-host target.
        missing = run(
            [gti, "build", "--target", cross_triple],
            expected=65,
            cwd=cross_project,
        )
        assert "GTI-B1904" in missing.stderr, missing.stderr
        assert "[toolchains." in missing.stderr, missing.stderr

        # A malformed triple is a usage error, not a build failure.
        malformed = run(
            [gti, "build", "--target", "not a triple"],
            expected=64,
            cwd=cross_project,
        )
        assert "invalid --target" in malformed.stderr, malformed.stderr

        # `check` performs no native compilation, so it analyzes another
        # supported target without a declared toolchain.
        checked = run(
            [gti, "check", "--target", cross_triple], cwd=cross_project
        )
        assert "Checked cross" in checked.stdout, checked.stdout

        # `run` executes on this machine and refuses a foreign target.
        refused = run(
            [gti, "run", "--target", cross_triple],
            expected=64,
            cwd=cross_project,
        )
        assert "gti build --target" in refused.stderr, refused.stderr

        if sys.platform != "win32":
            # A declared toolchain reaches the native invocation. The stub
            # compiler records its exact argument vector, so the assertions
            # below are about the command GTI built, not about having a real
            # cross toolchain installed on the test machine.
            record = cross_project / "native-commands.txt"
            stub = cross_project / "stub-c++"
            stub.write_text(
                "#!/usr/bin/env python3\n"
                "import pathlib, sys\n"
                f"record = pathlib.Path(r{str(record)!r})\n"
                "arguments = sys.argv[1:]\n"
                'record.open("a", encoding="utf-8").write('
                '"\\n".join(arguments) + "\\n")\n'
                'if "--version" in arguments:\n'
                '    print("gti stub cross compiler 1.0")\n'
                "    sys.exit(0)\n"
                "output = None\n"
                "for index, value in enumerate(arguments):\n"
                '    if value == "-o" and index + 1 < len(arguments):\n'
                "        output = arguments[index + 1]\n"
                "if output is not None:\n"
                "    produced = pathlib.Path(output)\n"
                "    produced.parent.mkdir(parents=True, exist_ok=True)\n"
                '    produced.write_bytes(b"#!/bin/sh\\nexit 0\\n")\n'
                "    produced.chmod(0o755)\n"
                "sys.exit(0)\n",
                encoding="utf-8",
            )
            stub.chmod(0o755)
            (cross_project / "gti.toml").write_text(
                host_only_manifest
                + f"\n[toolchains.{cross_triple}]\n"
                + f'cxx = "{stub}"\n'
                + 'sysroot = "sysroot"\n'
                + f'native-target = "{cross_triple}"\n',
                encoding="utf-8",
            )
            run([gti, "build", "--target", cross_triple], cwd=cross_project)
            recorded = record.read_text(encoding="utf-8").splitlines()
            assert f"--target={cross_triple}" in recorded, recorded
            sysroot = (cross_project / "sysroot").resolve()
            assert f"--sysroot={sysroot}" in recorded, recorded

            # The cross artifact publishes beside, not over, the host one.
            cross_directories = sorted(
                path.name
                for path in (cross_project / "build/gti/dev").iterdir()
                if path.is_dir()
            )
            assert len(cross_directories) == 2, cross_directories

            # A manifest `cc` remains optional when the user supplies the
            # higher-precedence command-line override. Without either source,
            # the cross build must still fail closed instead of discovering a
            # host C compiler.
            (cross_project / "native").mkdir()
            (cross_project / "native/support.c").write_text(
                "int support(void) { return 0; }\n", encoding="utf-8"
            )
            (cross_project / "gti.toml").write_text(
                host_only_manifest
                + '\n[package.native]\nc-sources = ["native/support.c"]\n'
                + f"\n[toolchains.{cross_triple}]\n"
                + f'cxx = "{stub}"\n'
                + 'sysroot = "sysroot"\n'
                + f'native-target = "{cross_triple}"\n',
                encoding="utf-8",
            )
            missing_cc = run(
                [gti, "build", "--target", cross_triple],
                expected=65,
                cwd=cross_project,
            )
            assert "GTI-B1905" in missing_cc.stderr, missing_cc.stderr
            record.write_text("", encoding="utf-8")
            run(
                [
                    gti,
                    "build",
                    "--target",
                    cross_triple,
                    "--cc",
                    str(stub),
                ],
                cwd=cross_project,
            )
            overridden_commands = record.read_text(encoding="utf-8").splitlines()
            assert str((cross_project / "native/support.c").resolve()) in (
                overridden_commands
            ), overridden_commands

            # An unknown toolchain field is rejected by the shared manifest
            # field validator.
            (cross_project / "gti.toml").write_text(
                host_only_manifest
                + f"\n[toolchains.{cross_triple}]\n"
                + f'cxx = "{stub}"\n'
                + 'linker = "ld"\n',
                encoding="utf-8",
            )
            unknown = run(
                [gti, "build", "--target", cross_triple],
                expected=65,
                cwd=cross_project,
            )
            assert "GTI-B1001" in unknown.stderr, unknown.stderr
        # --- Recorded build provenance and verification ---
        verify_project = root / "verify-project"
        (verify_project / "src").mkdir(parents=True)
        (verify_project / "gti.toml").write_text(
            "manifest-version = 1\n\n"
            "[package]\n"
            'name = "verify-project"\n'
            'version = "0.1.0"\n\n'
            "[targets.verify-project]\n"
            'kind = "executable"\n'
            'root = "src/main.gti"\n',
            encoding="utf-8",
        )
        (verify_project / "src/support.gti").write_text(
            "int support_value() { return 0; }\n", encoding="utf-8"
        )
        (verify_project / "src/main.gti").write_text(
            '#include "support.gti"\n'
            "int main() { return support_value(); }\n",
            encoding="utf-8",
        )
        report = verify_project / "provenance.json"
        recorded = run(
            [gti, "build", "--build-report", str(report)], cwd=verify_project
        )
        assert "Build report" in recorded.stdout
        record = json.loads(report.read_text(encoding="utf-8"))
        assert record["schema"] == 2
        project_section = record["project"]
        assert project_section["package_name"] == "verify-project"
        assert project_section["package_version"] == "0.1.0"
        assert project_section["target_name"] == "verify-project"
        assert project_section["target_kind"] == "executable"
        assert project_section["profile_name"] == "dev"
        # The record names the target it was built for, so `gti verify` can
        # reproduce a cross build instead of assuming the build host.
        recorded_triple = recorded.stdout.split("[dev, ", 1)[1].split("]", 1)[0]
        assert project_section["target_triple"] == recorded_triple
        assert project_section["model_identity"]
        verify_built = executable_named(
            verify_project / "build/gti/dev", "verify-project"
        )
        assert record["artifact"]["sha256"] == hashlib.sha256(
            verify_built.read_bytes()
        ).hexdigest()

        # An unchanged tree reproduces every recorded identity, and
        # verification never rewrites the record it is checking.
        preserved_record = report.read_bytes()
        verified = run(
            [gti, "verify", "--build-report", str(report), "-v"],
            cwd=verify_project,
        )
        assert "Verified verify-project [dev," in verified.stdout
        assert "reproduced:" in verified.stdout
        # Capture always performs the native invocation it describes.
        assert "cache bypassed" in verified.stderr
        assert report.read_bytes() == preserved_record

        # A changed source unit is named with its recorded and rebuilt
        # content identities, and the rebuilt executable differs too.
        (verify_project / "src/support.gti").write_text(
            "int support_value() { return 0; }\n"
            "int support_extra() { return 1; }\n",
            encoding="utf-8",
        )
        diverged = run(
            [gti, "verify", "--build-report", str(report)],
            expected=70,
            cwd=verify_project,
        )
        assert "does not describe this tree" in diverged.stderr
        assert "src/support.gti" in diverged.stderr
        assert "source materials: aggregate sha256" in diverged.stderr
        assert "built artifact: content" in diverged.stderr
        assert "src/main.gti" not in diverged.stderr
        assert report.read_bytes() == preserved_record

        # The record names the selection, so a contradicting one is refused,
        # and the option only belongs to commands that build one executable.
        run([gti, "verify"], expected=64, cwd=verify_project)
        run(
            [gti, "verify", "--build-report", str(report), "verify-project"],
            expected=64,
            cwd=verify_project,
        )
        run(
            [gti, "verify", "--build-report", str(report), "--release"],
            expected=64,
            cwd=verify_project,
        )
        run(
            [gti, "build", "--all", "--build-report", str(report)],
            expected=64,
            cwd=verify_project,
        )
        run(
            [gti, "check", "--build-report", str(report)],
            expected=64,
            cwd=verify_project,
        )
        run(
            [gti, "build", "--emit-mir", "--build-report", str(report)],
            expected=64,
            cwd=verify_project,
        )
        unreadable_record = verify_project / "unreadable.json"
        unreadable_record.write_text("{}\n", encoding="utf-8")
        malformed = run(
            [gti, "verify", "--build-report", str(unreadable_record)],
            expected=64,
            cwd=verify_project,
        )
        assert "is not a build report this compiler can verify" in (
            malformed.stderr
        )
        assert report.read_bytes() == preserved_record

        # `gti verify` takes every selection from the record, the target
        # triple included; a contradicting one would verify another build.
        rejected_triple = run(
            [
                gti,
                "verify",
                "--target",
                recorded_triple,
                "--build-report",
                str(report),
            ],
            expected=64,
            cwd=verify_project,
        )
        assert "takes its package, target, profile, and target triple" in (
            rejected_triple.stderr
        ), rejected_triple.stderr


if __name__ == "__main__":
    main()
