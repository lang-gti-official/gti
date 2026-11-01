#!/usr/bin/env python3
"""Dependency-owned nominal types retain one identity across compiled libraries."""
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
    example = Path(__file__).resolve().parents[1] / "examples/compiled-type-forwarding"
    env = os.environ.copy()
    for key in DEPENDENCY_INJECTING_NATIVE_ENVIRONMENT:
        env.pop(key, None)
    report = {"compiler_sha256": hashlib.sha256(compiler.read_bytes()).hexdigest(), "steps": []}

    def run(root, arguments, name, code=None, fragment=None):
        started = time.monotonic()
        done = subprocess.run([str(compiler), *arguments, "--color=never"], cwd=root,
                              env=env, text=True, capture_output=True, timeout=120)
        log = output / (name + ".log")
        log.write_text(done.stdout + done.stderr)
        report["steps"].append({"name": name, "exit": done.returncode,
                                "seconds": time.monotonic() - started, "log": str(log)})
        (output / "evidence.json").write_text(json.dumps(report, indent=2) + "\n")
        assert (done.returncode == 0) == (code is None), f"{name}: {log}"
        if code:
            assert code in done.stdout + done.stderr, f"{name}: expected {code}; {log}"
        if fragment:
            assert fragment in done.stdout + done.stderr, f"{name}: expected {fragment}; {log}"

    with tempfile.TemporaryDirectory(prefix="gti-type-forwarding-") as scratch:
        for level in (0, 1, 3):
            root = Path(scratch) / str(level)
            producer, consumer = root / "producer", root / 'consumer "relocated"'
            shutil.copytree(example, producer)
            profile = f"\n[profiles.release]\noptimization = {level}\n"
            manifest = producer / "gti.toml"
            manifest.write_text(manifest.read_text() + profile)
            prefix = f"O{level}-"
            run(producer, ["run", "app", "--release"], prefix + "automatic-diamond")
            names = ("geometry", "drawing", "processing", "inspection")
            bundles = {name: next((producer / "build").rglob(name + ".gtilib")) for name in names}
            original = {name: (path.read_bytes(), path.stat().st_mtime_ns) for name, path in bundles.items()}
            source = (producer / "main.gti").read_text()
            (producer / "main.gti").write_text(source + "\n// Consumer-only change.\n")
            run(producer, ["build", "app", "--release"], prefix + "reuse")
            assert all((path.read_bytes(), path.stat().st_mtime_ns) == original[name]
                       for name, path in bundles.items()), "consumer edit rebuilt a library"
            geometry = producer / "geometry.gti"
            geometry.write_text(geometry.read_text().replace("int value = 91", "int64_t value = 91"))
            run(producer, ["run", "app", "--release"], prefix + "private-layout-invalidation")
            changed = {name: path.read_bytes() for name, path in bundles.items()}
            assert all(changed[name] != original[name][0] for name in names), "stale type closure reused"
            (consumer / "dependencies").mkdir(parents=True)
            # Reorder artifacts and relocate the defining facade to an escaped path.
            interfaces = {"geometry": 'api/owning types/model "quoted".gti',
                          "drawing": "api/drawing.gti", "processing": "api/processing.gti",
                          "inspection": "api/inspection.gti"}
            header = ('manifest-version = 1\n[package]\nname = "forwarding-consumer"\n'
                      'version = "0.1.0"\n[targets.app]\nkind = "executable"\nroot = "main.gti"\n')
            entries = {}
            for name in ("inspection", "processing", "geometry", "drawing"):
                shutil.copyfile(bundles[name], consumer / "dependencies" / (name + ".gtilib"))
                entries[name] = (f'[[targets.app.compiled-libraries]]\npackage = "compiled-type-forwarding"\n'
                                 f'target = "{name}"\npath = "dependencies/{name}.gtilib"\n'
                                 f'interface = {json.dumps(interfaces[name])}\n')
            manifest = consumer / "gti.toml"
            distributed = header + "".join(entries.values()) + profile
            manifest.write_text(distributed)
            entry = consumer / "main.gti"
            entry.write_text(source)
            shutil.rmtree(producer)
            run(consumer, ["check", "app", "--release"], prefix + "source-free-check")
            run(consumer, ["run", "app", "--release"], prefix + "source-free-run")
            entry.write_text('import "api/processing.gti"; int main() { processing::Envelope value{}; return value.packet.point.x - 3; }')
            run(consumer, ["run", "app", "--release"], prefix + "types-only-no-imported-calls")
            import_geometry = "import " + json.dumps(interfaces["geometry"]) + ";\n"
            direct = (import_geometry + 'import "api/drawing.gti";\nint main() { '
                      'mut geometry::Point value{}; geometry::Point copy = drawing::point(value); '
                      '{ mut geometry::Point& loan = drawing::edit(value); loan.x = 21; } '
                      'if (value.x != 21 || copy.x != 3) { return 1; } return 0; }\n')
            entry.write_text(direct)
            run(consumer, ["run", "app", "--release"], prefix + "direct-owner-identity")
            entry.write_text(import_geometry + 'import "api/drawing.gti"; int main() { '
                             'drawing::Packet packet{}; drawing::Packet copy = drawing::echo(packet); '
                             'if (packet.status != geometry::Status::ready || copy.status != geometry::Status::ready || '
                             'drawing::status(copy.status) != geometry::Status::ready) { return 1; } return 0; }')
            run(consumer, ["run", "app", "--release"], prefix + "foreign-enum-default-and-call")
            negatives = {
                "no-implicit-reexport": ('int main() { geometry::Point value{}; return 0; }', "GTI-S"),
                "private-field": ('int main() { processing::Envelope value{}; return value.packet.point.stamp.value; }', "GTI-S"),
                "wrong-nominal": ('struct Point { int x = 3; int y = 4; }; int main() { Point value{}; return inspection::sum(value); }', "GTI-S"),
                "borrow-conflict": ('int main() { mut processing::Envelope value{}; processing::Envelope& read = value; mut processing::Envelope& write = processing::edit(value); write.packet.point.x = 4; return read.packet.point.x; }', "GTI-S2017"),
            }
            for name, (body, code) in negatives.items():
                entry.write_text('import "api/processing.gti"; import "api/inspection.gti";\n' + body)
                run(consumer, ["check", "app", "--release"], prefix + name, code)
            entry.write_text(import_geometry + 'int main() { geometry::Stamp value{}; return 0; }')
            run(consumer, ["check", "app", "--release"], prefix + "private-type", "GTI-S")
            entry.write_text(source)
            manifest.write_text(header + "".join(text for name, text in entries.items() if name != "geometry") + profile)
            run(consumer, ["check", "app", "--release"], prefix + "missing-owner", "gti: compiled library:", "missing compiled dependency artifact")
            manifest.write_text(distributed)
            drawing = consumer / "dependencies/drawing.gtilib"
            drawing.write_bytes(original["drawing"][0])
            run(consumer, ["check", "app", "--release"], prefix + "stale-owner-receipt", "gti: compiled library:", "conflicting compiled dependency artifact digest")
            drawing.write_bytes(changed["drawing"])
            assert not (consumer / "api").exists()
            assert not list(consumer.glob(".gti-compiled-*.gti"))
            assert all((consumer / "dependencies" / (name + ".gtilib")).read_bytes() == changed[name]
                       for name in names)
    print(output / "evidence.json")


if __name__ == "__main__":
    main()
