#!/usr/bin/env python3
"""Distribute and consume multiple independent GTI libraries without sources."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import time

from project_cli_smoke_test import DEPENDENCY_INJECTING_NATIVE_ENVIRONMENT


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("compiler", type=Path)
    parser.add_argument("--output-dir", required=True, type=Path)
    args = parser.parse_args()
    compiler = args.compiler.resolve()
    output = args.output_dir.resolve()
    output.mkdir(parents=True, exist_ok=True)
    example = Path(__file__).resolve().parents[1] / "examples/compiled-libraries"
    env = os.environ.copy()
    for key in DEPENDENCY_INJECTING_NATIVE_ENVIRONMENT:
        env.pop(key, None)
    report = {"compiler_sha256": hashlib.sha256(compiler.read_bytes()).hexdigest(), "steps": []}
    with tempfile.TemporaryDirectory(prefix="gti-compiled-libraries-") as temporary:
        root = Path(temporary)
        for name in ("left", "right", "consumer"):
            shutil.copytree(example / name, root / name)
        consumer = root / "consumer"
        dependencies = consumer / "dependencies"
        dependencies.mkdir()
        no_tools = root / "no-native-tools"
        no_tools.mkdir()
        check_env = dict(env, PATH=str(no_tools), CC=str(no_tools / "cc"), CXX=str(no_tools / "c++"))

        def snapshot(project):
            return {str(p.relative_to(project)): (p.stat().st_mtime_ns, hashlib.sha256(p.read_bytes()).hexdigest())
                    for p in project.rglob("*") if p.is_file()}

        def run(project, arguments, name, success=True, contains=None):
            started = time.monotonic()
            result = subprocess.run([str(compiler), *arguments, "--color=never"],
                                    cwd=project, env=check_env if arguments[0] == "check" else env,
                                    capture_output=True, text=True, timeout=120)
            log = output / (name + ".log")
            log.write_text(result.stdout + result.stderr)
            report["steps"].append({"name": name, "exit": result.returncode,
                                    "seconds": time.monotonic() - started, "log": str(log)})
            (output / "evidence.json").write_text(json.dumps(report, indent=2) + "\n")
            assert (result.returncode == 0) == success, f"{name}: exit {result.returncode}; {log}"
            if contains:
                assert contains in result.stdout + result.stderr, f"{name}: missing {contains!r}; {log}"
            return result

        for name in ("left", "right"):
            producer = root / name
            run(producer, ["build", name, "--release"], "build-" + name)
            bundles = list((producer / "build").rglob(name + ".gtilib"))
            assert len(bundles) == 1, bundles
            shutil.copyfile(bundles[0], dependencies / (name + ".gtilib"))
            if name == "right":
                # Same spelling/signature, a different artifact owner and result.
                for filename in ("library.gti", "gti.toml"):
                    path = producer / filename
                    path.write_text(path.read_text().replace("right_value", "left_value"))
                run(producer, ["build", name, "--release"], "build-overlapping-right")
                shutil.copyfile(bundles[0], root / "overlap.gtilib")
            shutil.rmtree(producer)
        assert not (root / "left").exists() and not (root / "right").exists()
        original_dependencies = snapshot(dependencies)
        command = ["build", "app", "--release", "--verbose"]
        check = ["check", "app", "--release"]
        before = snapshot(consumer)
        run(consumer, check, "check-without-producers-or-tools", contains="Checked app")
        assert snapshot(consumer) == before and not (consumer / "build").exists()
        metadata = json.loads(run(consumer, ["metadata"], "metadata").stdout)
        target = next(t for t in metadata["targets"] if t["name"] == "app")
        assert [i["package"] for i in target["compiledLibraries"]] == ["compiled-left", "compiled-right"]
        assert "compiledLibrary" not in target
        run(consumer, command, "build-two-libraries", contains="cache bypassed")
        run(consumer, ["run", "app", "--release", "--no-build"], "execute-two-libraries")
        for name in ("left.gti", "right.gti"):
            assert not (consumer / name).exists(), "virtual interface leaked to disk"
        source = consumer / "main.gti"
        original_source = source.read_text()
        source.write_text(original_source.replace("return 0;", "return left_value(1) + right_value(1);"))
        run(consumer, command, "consumer-only-edit")
        run(consumer, ["run", "app", "--release", "--no-build"], "execute-edited")
        assert snapshot(dependencies) == original_dependencies, "consumer rebuilt a dependency"
        executable = next(p for p in (consumer / "build").rglob("app") if p.is_file())
        executable_bytes = executable.read_bytes()
        artifact = dependencies / "right.gtilib"
        original_artifact = artifact.read_bytes()
        artifact.write_bytes(original_artifact[:-1] + bytes([original_artifact[-1] ^ 1]))
        run(consumer, command, "corrupt-second-library", False, "compiled library")
        run(consumer, check, "check-corrupt-second-library", False, "compiled library")
        assert executable.read_bytes() == executable_bytes
        # Selection validates every dependency, even if the source imports only one.
        source.write_text('import "left.gti"; int main() { return left_value(1); }\n')
        run(consumer, check, "check-unused-corrupt-library", False, "compiled library")
        artifact.write_bytes(original_artifact)
        source.write_text(original_source)
        # Each individual file may fit the envelope cap while their sum does
        # not. Reject the second stat before allocating or reading its payload.
        with artifact.open("wb") as oversized:
            oversized.truncate(256 * 1024 * 1024)
        run(consumer, check, "check-combined-byte-limit", False, "combined compiled libraries")
        run(consumer, command, "combined-byte-limit", False, "combined compiled libraries")
        assert executable.read_bytes() == executable_bytes
        artifact.write_bytes(original_artifact)
        manifest = consumer / "gti.toml"
        original_manifest = manifest.read_text()
        for name, changed, fragment in [
            ("duplicate-owner", original_manifest.replace('package = "compiled-right"', 'package = "compiled-left"')
                .replace('target = "right"', 'target = "left"'), "owners must be unique"),
            ("duplicate-interface", original_manifest.replace('interface = "right.gti"', 'interface = "left.gti"'),
                "paths must be unique"),
            ("wrong-second-identity", original_manifest.replace('package = "compiled-right"', 'package = "wrong"'),
                "does not match"),
        ]:
            manifest.write_text(changed)
            run(consumer, check, "check-" + name, False, fragment)
            run(consumer, command, name, False, fragment)
            assert executable.read_bytes() == executable_bytes
        prefix, left, right = original_manifest.split("[[targets.app.compiled-libraries]]")
        manifest.write_text(prefix + "[[targets.app.compiled-libraries]]" + right +
                            "[[targets.app.compiled-libraries]]" + left)
        run(consumer, command, "reordered-imports")
        run(consumer, ["run", "app", "--release", "--no-build"], "execute-reordered")
        manifest.write_text(original_manifest)
        # Valid selected libraries need not be imported by a source unit.
        source.write_text('int main() { return 0; }\n')
        run(consumer, check, "check-all-selected-libraries-unused")
        run(consumer, command, "build-all-selected-libraries-unused")
        run(consumer, ["run", "app", "--release", "--no-build"], "execute-unused-libraries")
        artifact.write_bytes((root / "overlap.gtilib").read_bytes())
        for facade, expected in (("left", 7), ("right", 35)):
            source.write_text(f'import "{facade}.gti"; int main() {{ '
                              f'if (left_value(0) != {expected}) {{ return 1; }} return 0; }}\n')
            run(consumer, check, "check-overlap-only-" + facade)
            run(consumer, command, "build-overlap-only-" + facade)
            run(consumer, ["run", "app", "--release", "--no-build"], "execute-overlap-only-" + facade)
        source.write_text('import "left.gti"; import "right.gti"; '
                          'int main() { return left_value(0); }\n')
        overlap_executable = executable.read_bytes()
        run(consumer, check, "check-overlap-ambiguous", False, "ambiguous")
        run(consumer, command, "build-overlap-ambiguous", False, "ambiguous")
        assert executable.read_bytes() == overlap_executable
        artifact.write_bytes(original_artifact)
        # A plural singleton retains the legacy metadata object for clients.
        manifest.write_text(prefix + "[[targets.app.compiled-libraries]]" + left)
        source.write_text('import "left.gti"; int main() { return left_value(1); }\n')
        target = next(t for t in json.loads(run(consumer, ["metadata"], "singleton-metadata").stdout)["targets"]
                      if t["name"] == "app")
        assert target["compiledLibraries"] == [target["compiledLibrary"]]
        run(consumer, check, "check-plural-singleton")
        run(consumer, command, "build-plural-singleton")
        run(consumer, ["run", "app", "--release", "--no-build"], "execute-plural-singleton")
    print("multiple compiled library project evidence:", output / "evidence.json")


if __name__ == "__main__":
    main()
