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
        assert metadata_document["schemaVersion"] == 1
        assert metadata_document["package"]["name"] == "sample"
        assert metadata_document["targets"][0]["outputs"][1]["profile"] == "release"
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

        bad_metadata_format = run(
            [gti, "metadata", "--format", "text"],
            expected=64,
            cwd=check_project,
        )
        assert "metadata format must be json" in bad_metadata_format.stderr

        run_project = root / "run-project"
        run_project.mkdir()
        (run_project / "main.gti").write_text(
            "int main() { return 7; }\n", encoding="utf-8"
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


if __name__ == "__main__":
    main()
