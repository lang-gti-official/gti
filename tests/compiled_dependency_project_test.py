#!/usr/bin/env python3
"""Build a compiled source DAG, retain exact receipts, and reuse unchanged units."""
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
    example = Path(__file__).resolve().parents[1] / "examples/compiled-dependencies"
    env = os.environ.copy()
    for key in DEPENDENCY_INJECTING_NATIVE_ENVIRONMENT:
        env.pop(key, None)
    report = {"compiler_sha256": hashlib.sha256(compiler.read_bytes()).hexdigest(), "steps": []}
    with tempfile.TemporaryDirectory(prefix="gti-compiled-dependencies-") as temporary:
        root = Path(temporary) / "project"
        shutil.copytree(example, root)
        no_tools = Path(temporary) / "no-tools"
        no_tools.mkdir()
        check_env = dict(env, PATH=str(no_tools), CC=str(no_tools / "cc"), CXX=str(no_tools / "c++"))

        def snapshot(directory):
            return {str(p.relative_to(directory)): (p.stat().st_mtime_ns, hashlib.sha256(p.read_bytes()).hexdigest())
                    for p in directory.rglob("*") if p.is_file()}

        def run(project, arguments, name, success=True, contains=None):
            started = time.monotonic()
            result = subprocess.run([str(compiler), *arguments, "--color=never"], cwd=project,
                                    env=check_env if arguments[0] == "check" else env,
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

        build = ["build", "app", "--release", "--verbose"]
        check = ["check", "app", "--release"]
        before = snapshot(root)
        run(root, check, "check-unbuilt", False, "build its library target first")
        assert snapshot(root) == before and not (root / "build").exists(), "check emitted outputs"
        run(root, build, "build-diamond")
        run(root, ["run", "app", "--release", "--no-build"], "execute-diamond")
        bundles = {name: next((root / "build").rglob(name + ".gtilib")) for name in ("base", "left", "right")}
        assert len(list((root / "build").rglob("*.gtilib"))) == 3
        assert not (root / "api").exists(), "virtual facades leaked to disk"
        original_bytes = {name: path.read_bytes() for name, path in bundles.items()}
        original_stat = {name: path.stat().st_mtime_ns for name, path in bundles.items()}
        before = snapshot(root)
        run(root, check, "check-built-without-native-tools", contains="Checked app")
        assert snapshot(root) == before, "check rewrote dependency outputs"
        run(root, build, "reuse-diamond", contains="cache hit")
        assert original_stat == {name: path.stat().st_mtime_ns for name, path in bundles.items()}, "unchanged libraries were rebuilt"
        source = root / "main.gti"
        source.write_text(source.read_text() + "\n// consumer-only source edit\n")
        run(root, build, "consumer-only-edit")
        assert original_stat == {name: path.stat().st_mtime_ns for name, path in bundles.items()}
        base = root / "base.gti"
        original_base = base.read_text()
        base.write_text(original_base.replace("return 7;", "return 8;"))
        source.write_text(source.read_text().replace("!= 42", "!= 48"))
        run(root, build, "dependency-edit")
        run(root, ["run", "app", "--release", "--no-build"], "execute-edited-dependency")
        assert all(original_bytes[name] != path.read_bytes() for name, path in bundles.items()), "dependency receipt did not invalidate a parent"
        changed_bytes = {name: path.read_bytes() for name, path in bundles.items()}
        executable = next(p for p in (root / "build").rglob("app") if p.is_file())
        executable_bytes = executable.read_bytes()
        bundles["left"].write_bytes(original_bytes["left"])
        run(root, check, "reject-stale-parent-receipt", False, "dependency artifact digest")
        run(root, build, "repair-stale-parent")
        assert bundles["left"].read_bytes() == changed_bytes["left"]
        bundles["base"].write_bytes(b"broken bundle")
        run(root, build, "repair-corrupt-source-dependency")
        assert bundles["base"].read_bytes() == changed_bytes["base"]
        base.write_text("export module base; export int base_value() { return missing; }\n")
        run(root, build, "dependency-frontend-failure", False, "missing")
        assert changed_bytes == {name: path.read_bytes() for name, path in bundles.items()}
        assert executable.read_bytes() == executable_bytes, "failed dependency replaced application"
        base.write_text(original_base.replace("return 7;", "return 8;"))
        manifest = root / "gti.toml"
        original_manifest = manifest.read_text()
        for name, changed, fragment in [
            ("cycle", original_manifest + '\n[[targets.base.compiled-dependencies]]\ntarget = "left"\ninterface = "api/left.gti"\n', "Compiled dependency cycle"),
            ("missing-target", original_manifest.replace('target = "base"', 'target = "absent"'), "must name a gti-library"),
            ("wrong-kind", original_manifest.replace('target = "base"', 'target = "app"'), "must name a gti-library"),
            ("duplicate-facade", original_manifest.replace('interface = "api/right.gti"', 'interface = "api/left.gti"'), "paths must be unique"),
            ("duplicate-target", original_manifest.replace('target = "right"', 'target = "left"'), "targets must be unique"),
        ]:
            manifest.write_text(changed)
            run(root, build, name, False, fragment)
            assert executable.read_bytes() == executable_bytes
        manifest.write_text(original_manifest)
        metadata = json.loads(run(root, ["metadata"], "metadata").stdout)
        target = next(t for t in metadata["targets"] if t["name"] == "app")
        assert [d["target"] for d in target["compiledDependencies"]] == ["left", "right"]
        # Distributed consumers retain the full closure but need no producer source.
        distributed = Path(temporary) / "distributed"
        distributed.mkdir()
        shutil.copyfile(source, distributed / "main.gti")
        for name, path in bundles.items():
            shutil.copyfile(path, distributed / (name + ".gtilib"))
        prefix = ('manifest-version = 1\n[package]\nname = "distributed"\nversion = "0.1.0"\n'
                  '[targets.app]\nkind = "executable"\nroot = "main.gti"\n')
        entries = {name: (f'[[targets.app.compiled-libraries]]\npackage = "compiled-dependencies"\n'
                          f'target = "{name}"\npath = "{name}.gtilib"\ninterface = "api/{name}.gti"\n')
                   for name in bundles}
        (distributed / "gti.toml").write_text(prefix + "".join(entries.values()))
        shutil.rmtree(root)
        run(distributed, check, "check-distributed-without-sources")
        run(distributed, build, "build-distributed-closure")
        run(distributed, ["run", "app", "--release", "--no-build"], "execute-distributed-closure")
        (distributed / "gti.toml").write_text(prefix + entries["left"] + entries["right"])
        run(distributed, check, "reject-incomplete-closure", False, "missing compiled dependency artifact")
    print("Compiled dependency DAG, receipts, source-less distribution and reuse checks passed")


if __name__ == "__main__":
    main()
