#!/usr/bin/env python3
"""Compiled reference APIs retain ordinary caller-local loans and access."""
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
    compiler, output = args.compiler.resolve(), args.output_dir.resolve()
    output.mkdir(parents=True, exist_ok=True)
    example = Path(__file__).resolve().parents[1] / "examples/compiled-borrowing"
    env = os.environ.copy()
    for key in DEPENDENCY_INJECTING_NATIVE_ENVIRONMENT:
        env.pop(key, None)
    report = {"compiler_sha256": hashlib.sha256(compiler.read_bytes()).hexdigest(), "steps": []}
    with tempfile.TemporaryDirectory(prefix="gti-compiled-borrow-") as temporary:
        root = Path(temporary)

        def run(project, command, name, success=True):
            started = time.monotonic()
            done = subprocess.run([str(compiler), *command, "--color=never"], cwd=project,
                                  env=env, capture_output=True, text=True, timeout=120)
            log = output / (name + ".log")
            log.write_text(done.stdout + done.stderr)
            report["steps"].append({"name": name, "exit": done.returncode,
                                    "seconds": time.monotonic() - started, "log": str(log)})
            (output / "evidence.json").write_text(json.dumps(report, indent=2) + "\n")
            assert (done.returncode == 0) == success, f"{name}: exit {done.returncode}; {log}"
            if not success:
                assert "GTI-S2017" in done.stdout + done.stderr, f"{name}: expected loan/access rejection; {log}"
            return done

        for level in (0, 3):
            work = root / str(level)
            shutil.copytree(example, work)
            producer, consumer = work / "producer", work / "consumer"
            for project in (producer, consumer):
                with (project / "gti.toml").open("a") as manifest:
                    manifest.write(f"\n[profiles.release]\noptimization = {level}\n")
            prefix = f"O{level}-"
            run(producer, ["check", "borrowing", "--release"], prefix + "producer-check")
            run(producer, ["build", "borrowing", "--release"], prefix + "producer-build")
            bundle = next((producer / "build").rglob("borrowing.gtilib"))
            dependencies = consumer / "dependencies"
            dependencies.mkdir()
            shutil.copyfile(bundle, dependencies / "borrowing.gtilib")
            original = (dependencies / "borrowing.gtilib").read_bytes()
            shutil.rmtree(producer)
            run(consumer, ["check", "app", "--release"], prefix + "consumer-check")
            run(consumer, ["run", "app", "--release"], prefix + "consumer-run")
            source = consumer / "main.gti"
            invalid = {
                "mutation-while-read-loan": 'int main() { mut int x = 1; int& v = read(x); store(x, 2); return v; }',
                "overlapping-mutable-loans": 'int main() { mut int x = 1; mut int& a = edit(x); mut int& b = edit(x); a = 2; return b; }',
                "mutable-from-immutable": 'int main() { int x = 1; mut int& v = edit(x); return v; }',
                "temporary-escape": 'int main() { int& v = read(7); return v; }',
                "local-escape": 'int& escape() { int x = 1; return read(x); } int main() { return 0; }',
                "second-argument-remains-borrowed": 'int main() { mut int first = 1; mut int second = 2; int& v = choose(first, second); store(second, 3); return v; }',
                "nominal-mutation-while-read-loan": 'int main() { mut Sample x = {.value = 1}; Sample& v = read_sample(x); mut Sample& changed = edit_sample(x); changed.value = 2; return v.value; }',
                "nominal-mutable-from-immutable": 'int main() { Sample x = {.value = 1}; mut Sample& v = edit_sample(x); return v.value; }',
            }
            for name, body in invalid.items():
                source.write_text('import "api.gti";\n' + body + "\n")
                run(consumer, ["check", "app", "--release"], prefix + name, False)
            assert (dependencies / "borrowing.gtilib").read_bytes() == original
            assert not (consumer / "api.gti").exists()
    print(output / "evidence.json")


if __name__ == "__main__":
    main()
