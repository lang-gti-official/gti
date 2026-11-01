#!/usr/bin/env python3
"""Build and distribute a GTI library through the real project commands."""
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
    example = Path(__file__).resolve().parents[1] / "examples/compiled-scalar-library"
    env = os.environ.copy()
    for key in DEPENDENCY_INJECTING_NATIVE_ENVIRONMENT:
        env.pop(key, None)
    report = {"compiler_sha256": hashlib.sha256(compiler.read_bytes()).hexdigest(), "steps": []}
    with tempfile.TemporaryDirectory(prefix="gti-compiled-project-") as temporary:
        root = Path(temporary)
        producer, consumer = root / "producer", root / "consumer"
        producer.mkdir()
        for name in ("gti.toml", "library.gti", "implementation.gti"):
            shutil.copyfile(example / name, producer / name)
        shutil.copytree(example / "consumer", consumer)

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

        producer_command = ["build", "numbers", "--release", "--verbose"]
        producer_check = ["check", "numbers", "--release"]
        before_check = snapshot(producer)
        run(producer, producer_check, "check-producer", contains="Checked numbers")
        assert snapshot(producer) == before_check and not (producer / "build").exists()
        run(producer, producer_command, "producer", contains="cache bypassed")
        bundles = list((producer / "build").rglob("numbers.gtilib"))
        assert len(bundles) == 1, bundles
        original = bundles[0].read_bytes()
        metadata = json.loads(run(producer, ["metadata"], "producer-metadata").stdout)
        target = next(t for t in metadata["targets"] if t["name"] == "numbers")
        assert target["kind"] == "gti-library" and target["functions"] == ["choose"]
        manifest = producer / "gti.toml"
        manifest_source = manifest.read_text()
        for name, replacement, fragment in [
            ("private-module-function", '["choose_impl"]', "explicitly exported by the root module"),
            ("missing-function", '["missing"]', "explicitly exported by the root module"),
            ("duplicate-function", '["choose", "choose"]', "GTI-B1005"),
            ("empty-functions", '[]', "GTI-B1005"),
        ]:
            manifest.write_text(manifest_source.replace('["choose"]', replacement))
            run(producer, producer_command, name, False, fragment)
            run(producer, producer_check, "check-" + name, False, fragment)
            assert bundles[0].read_bytes() == original, "failed producer replaced the good bundle"
        manifest.write_text(manifest_source)
        library_source = producer / "library.gti"
        original_source = library_source.read_text()
        for name, changed, fragment in [
            ("private-root-function", original_source.replace("export int choose", "int choose"), "explicitly exported"),
            ("missing-module", original_source.replace("export module numbers;", ""), "module interface"),
        ]:
            library_source.write_text(changed)
            run(producer, producer_command, name, False, fragment)
            run(producer, producer_check, "check-" + name, False, fragment)
            assert bundles[0].read_bytes() == original
        library_source.write_text(original_source)
        manifest.write_text(manifest_source + '\n[package.native]\nlibraries = ["m"]\n')
        run(producer, producer_command, "producer-native-inputs", False, "self-contained")
        run(producer, producer_check, "check-producer-native-inputs", False, "self-contained")
        manifest.write_text(manifest_source)
        (producer / "native.h").write_text("int native_value(void);\n")
        manifest.write_text(manifest_source + '\n[bindgen.native]\nheader = "native.h"\n')
        run(producer, producer_command, "producer-bindgen", False, "do not support bindgen")
        run(producer, producer_check, "check-producer-bindgen", False, "does not run bindgen")
        assert not list((producer / "build").rglob("*native*.gti"))
        manifest.write_text(manifest_source)
        run(producer, ["build", "--all", "--release"], "producer-build-all")
        run(producer, [*producer_command, "--backend", "cpp"], "producer-cpp", False, "requires LLVM")
        run(producer, ["run", "numbers", "--release"], "library-is-not-executable", False, "not an executable")

        artifact = consumer / "dependencies/numbers.gtilib"
        artifact.parent.mkdir()
        shutil.copyfile(bundles[0], artifact)
        shutil.rmtree(producer)
        assert not producer.exists()
        command = ["build", "app", "--release", "--verbose"]
        consumer_check = ["check", "app", "--release"]
        before_check = snapshot(consumer)
        run(consumer, consumer_check, "check-source-absent", contains="Checked app")
        assert snapshot(consumer) == before_check and not (consumer / "build").exists()
        run(consumer, command, "source-absent-consumer", contains="cache bypassed")
        run(consumer, ["run", "app", "--release", "--no-build"], "execute")
        assert not (consumer / "api.gti").exists(), "virtual interface leaked to disk"
        before = artifact.stat().st_mtime_ns
        (consumer / "main.gti").write_text('import "api.gti"; int main() { return choose(0); }\n')
        run(consumer, command, "consumer-only-edit")
        run(consumer, ["run", "app", "--release", "--no-build"], "execute-edited")
        assert artifact.read_bytes() == original and artifact.stat().st_mtime_ns == before
        executable = next(p for p in (consumer / "build").rglob("app") if p.is_file())
        executable_bytes = executable.read_bytes()

        artifact.write_bytes(original[:-1] + bytes([original[-1] ^ 1]))
        run(consumer, command, "corrupt-artifact", False, "compiled library")
        run(consumer, consumer_check, "check-corrupt-artifact", False, "compiled library")
        assert executable.read_bytes() == executable_bytes
        with artifact.open("wb") as file:
            file.truncate(256 * 1024 * 1024 + 1)
        run(consumer, command, "oversized-artifact", False, "size limit")
        run(consumer, consumer_check, "check-oversized-artifact", False, "size limit")
        artifact.write_bytes(original)
        manifest = consumer / "gti.toml"
        source = manifest.read_text()
        manifest.write_text(source.replace('package = "compiled-numbers"', 'package = "wrong-package"'))
        run(consumer, command, "wrong-package", False, "does not match")
        run(consumer, consumer_check, "check-wrong-package", False, "does not match")
        manifest.write_text(source.replace('interface = "api.gti"', 'interface = "../escape.gti"'))
        run(consumer, command, "escaping-interface", False, "GTI-B1005")
        manifest.write_text(source)
        (consumer / "api.gti").write_text('export module forged; export int choose(int x);\n')
        run(consumer, command, "existing-interface", False, "GTI-B1005")
        (consumer / "api.gti").unlink()
        before_check = snapshot(consumer)
        run(consumer, consumer_check, "check-existing-build", contains="Checked app")
        run(consumer, ["check", "app"], "check-incompatible-optimization", False, "configuration")
        assert snapshot(consumer) == before_check
        main = consumer / "main.gti"
        original_main = main.read_text()
        for name, source_text in [
            ("bad-argument", 'import "api.gti"; int main() { return choose(true); }\n'),
            ("private-helper", 'import "api.gti"; int main() { return choose_impl(0); }\n'),
            ("ordinary-declaration", 'import "api.gti"; int missing(int x); int main() { return missing(0); }\n'),
        ]:
            main.write_text(source_text)
            checked = run(consumer, consumer_check, "check-" + name, False)
            assert "GTI-" in checked.stderr and "main.gti" in checked.stderr
            run(consumer, command, "build-" + name, False)
        main.write_text(original_main)
        run(consumer, [*command, "--emit-mir"], "mir-boundary", False, "artifact-aware validation")
        run(consumer, [*command, "--build-report", str(root / "report.json")], "report-boundary", False, "build reports")
        assert executable.read_bytes() == executable_bytes
        assert not (root / "report.json").exists()
        linker = root / "failing native linker"
        linker.write_text("#!/bin/sh\nexit 27\n")
        linker.chmod(0o700)
        run(consumer, [*command, "--cxx", str(linker)], "failed-native-link", False)
        assert executable.read_bytes() == executable_bytes, "failed link replaced the executable"
        assert not list((consumer / "build").rglob("*.gtilib.o")), "staged objects were not cleaned"
        saved_build, outside = consumer / "saved-build", root / "outside"
        (consumer / "build").rename(saved_build)
        outside.mkdir()
        sentinel = outside / "sentinel"
        sentinel.write_text("preserve")
        (consumer / "build").symlink_to(outside, target_is_directory=True)
        run(consumer, command, "symlinked-output-root", False)
        assert list(outside.iterdir()) == [sentinel] and sentinel.read_text() == "preserve"
        (consumer / "build").unlink()
        saved_build.rename(consumer / "build")
    print(f"compiled library project workflow passed; evidence: {output}")


if __name__ == "__main__":
    main()
