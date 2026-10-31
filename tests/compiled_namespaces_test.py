#!/usr/bin/env python3
"""Namespace identity, visibility and borrowing across source-free libraries."""
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
    parser.add_argument("--output-dir", type=Path, required=True)
    args = parser.parse_args()
    compiler, output = args.compiler.resolve(), args.output_dir.resolve()
    output.mkdir(parents=True, exist_ok=True)
    fixture = Path(__file__).resolve().parents[1] / "examples/compiled-namespaces"
    environment = os.environ.copy()
    for key in DEPENDENCY_INJECTING_NATIVE_ENVIRONMENT:
        environment.pop(key, None)
    report = {"compiler_sha256": hashlib.sha256(compiler.read_bytes()).hexdigest(), "steps": []}

    def run(project, arguments, label, code=None, fragment=None):
        started = time.monotonic()
        done = subprocess.run([str(compiler), *arguments, "--color=never"], cwd=project,
                              env=environment, text=True, capture_output=True, timeout=120)
        log = output / (label + ".log")
        text = done.stdout + done.stderr
        log.write_text(text)
        report["steps"].append({"name": label, "exit": done.returncode,
                                "seconds": time.monotonic() - started, "log": str(log)})
        (output / "evidence.json").write_text(json.dumps(report, indent=2) + "\n")
        assert (done.returncode == 0) == (code is None), f"{label}: {log}"
        if code:
            assert code in text, f"{label}: expected {code}; {log}"
        if fragment:
            assert fragment in text, f"{label}: expected {fragment}; {log}"

    with tempfile.TemporaryDirectory(prefix="gti-compiled-namespaces-") as scratch:
        for level in (0, 1, 3):
            root = Path(scratch) / str(level)
            producer, consumer = root / "producer", root / "consumer"
            shutil.copytree(fixture, producer)
            profile = f"\n[profiles.release]\noptimization = {level}\n"
            manifest = producer / "gti.toml"
            manifest.write_text(manifest.read_text() + profile)
            original_manifest = manifest.read_text()
            prefix = f"O{level}-"
            run(producer, ["run", "app", "--release"], prefix + "automatic-build-run")
            bundles = {name: next((producer / "build").rglob(name + ".gtilib"))
                       for name in ("geometry", "telemetry")}
            unchanged = {name: (path.read_bytes(), path.stat().st_mtime_ns)
                         for name, path in bundles.items()}
            original_source = (producer / "main.gti").read_text()
            (producer / "main.gti").write_text(original_source + "\n// Consumer-only edit.\n")
            run(producer, ["build", "app", "--release"], prefix + "reuse")
            assert all((path.read_bytes(), path.stat().st_mtime_ns) == unchanged[name]
                       for name, path in bundles.items()), "consumer edit rebuilt a library"
            manifest.write_text(original_manifest.replace('"domain::geometry::make"',
                                                        '"domain::geometry::secret"'))
            run(producer, ["build", "geometry", "--release"], prefix + "private-selection",
                "GTI-B0003", "exported by the root module")
            manifest.write_text(original_manifest.replace('"domain::geometry::make"',
                                                        '"domain:::geometry::make"'))
            run(producer, ["build", "geometry", "--release"], prefix + "malformed-selection",
                "GTI-B1005", "qualified by namespaces")
            assert all(path.read_bytes() == unchanged[name][0] for name, path in bundles.items())
            (consumer / "dependencies").mkdir(parents=True)
            distributed = ('manifest-version = 1\n[package]\nname = "namespace-consumer"\n'
                           'version = "0.1.0"\n[targets.app]\nkind = "executable"\nroot = "main.gti"\n')
            for name, path in bundles.items():
                shutil.copyfile(path, consumer / "dependencies" / (name + ".gtilib"))
                distributed += (f'[[targets.app.compiled-libraries]]\npackage = "compiled-namespaces"\n'
                                f'target = "{name}"\npath = "dependencies/{name}.gtilib"\n'
                                f'interface = "api/{name}.gti"\n')
            (consumer / "gti.toml").write_text(distributed + profile)
            source = consumer / "main.gti"
            source.write_text(original_source)
            shutil.rmtree(producer)
            run(consumer, ["check", "app", "--release"], prefix + "source-free-check")
            run(consumer, ["run", "app", "--release"], prefix + "source-free-run")
            negatives = {
                "private-type": ('int main() { domain::detail::Point value{}; return 0; }', "GTI-S"),
                "private-function": ('int main() { return domain::geometry::secret(); }', "GTI-S"),
                "private-overload": ('int main() { return domain::telemetry::count(true); }', "GTI-S"),
                "wrong-nominal": ('int main() { domain::telemetry::Point value{}; domain::geometry::Point wrong = domain::geometry::echo(value); return 0; }', "GTI-S"),
                "borrow-conflict": ('int main() { mut domain::geometry::Point value = domain::geometry::make(1, 2); domain::geometry::Point& loan = domain::geometry::view(value); mut domain::geometry::Point& write = domain::geometry::edit(value); write.x = 3; return loan.x; }', "GTI-S2017"),
            }
            for name, (body, code) in negatives.items():
                source.write_text('import "api/geometry.gti"; import "api/telemetry.gti";\n' + body + "\n")
                run(consumer, ["check", "app", "--release"], prefix + name, code)
            assert not (consumer / "api").exists()
            assert all((consumer / "dependencies" / (name + ".gtilib")).read_bytes() == value[0]
                       for name, value in unchanged.items())
    print(output / "evidence.json")


if __name__ == "__main__":
    main()
