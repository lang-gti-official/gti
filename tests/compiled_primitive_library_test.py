#!/usr/bin/env python3
"""Primitive/void/overloaded compiled APIs and checked failure propagation."""
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
    example = Path(__file__).resolve().parents[1] / "examples/compiled-primitive-library"
    env = os.environ.copy()
    for key in DEPENDENCY_INJECTING_NATIVE_ENVIRONMENT:
        env.pop(key, None)
    report = {"compiler_sha256": hashlib.sha256(compiler.read_bytes()).hexdigest(), "steps": []}
    with tempfile.TemporaryDirectory(prefix="gti-primitive-library-") as temporary:
        root = Path(temporary)

        def run(project, command, name, code=0, contains=None):
            started = time.monotonic()
            done = subprocess.run([str(compiler), *command, "--color=never"], cwd=project,
                                  env=env, capture_output=True, text=True, timeout=120)
            log = output / (name + ".log")
            log.write_text(done.stdout + done.stderr)
            report["steps"].append({"name": name, "exit": done.returncode,
                                    "seconds": time.monotonic() - started, "log": str(log)})
            (output / "evidence.json").write_text(json.dumps(report, indent=2) + "\n")
            assert done.returncode == code, f"{name}: exit {done.returncode}; {log}"
            if contains:
                assert contains in done.stdout + done.stderr, f"{name}: missing {contains!r}; {log}"
            return done

        for level in (0, 1, 3):
            work = root / str(level)
            shutil.copytree(example, work)
            producer, consumer = work / "producer", work / "consumer"
            for project in (producer, consumer):
                with (project / "gti.toml").open("a") as manifest:
                    manifest.write(f"\n[profiles.release]\noptimization = {level}\n")
            prefix = f"O{level}-"
            run(producer, ["check", "primitives", "--release"], prefix + "producer-check")
            run(producer, ["build", "primitives", "--release"], prefix + "producer-build")
            bundle = next((producer / "build").rglob("primitives.gtilib"))
            library_source = producer / "library.gti"
            original_source = library_source.read_text()
            primary_line = next(i for i, line in enumerate(original_source.splitlines(), 1)
                                if "return value / divisor" in line)
            secondary_line = next(i for i, line in enumerate(original_source.splitlines(), 1)
                                  if "int result = value / divisor" in line)
            dependencies = consumer / "dependencies"
            dependencies.mkdir()
            shutil.copyfile(bundle, dependencies / "primitives.gtilib")
            original = (dependencies / "primitives.gtilib").read_bytes()
            # A producer that observes failure cleanup must retain its runtime
            # context checks and remains outside the self-contained profile.
            library_source.write_text(original_source.replace(
                "export int divide(int value, int divisor) { return value / divisor; }",
                '''class ProducerMarker {
public:
    ProducerMarker() {}
    ~ProducerMarker() { std::println("producer cleanup"); }
};
export int divide(int value, int divisor) {
    ProducerMarker marker{};
    return value / divisor;
}'''))
            run(producer, ["build", "primitives", "--release"],
                prefix + "observable-producer-cleanup-rejected", 65,
                "unresolved native dependency")
            assert bundle.read_bytes() == original, "rejected producer replaced artifact"
            shutil.rmtree(producer)
            run(consumer, ["check", "app", "--release"], prefix + "consumer-check")
            run(consumer, ["run", "app", "--release"], prefix + "all-primitive-results")
            source = consumer / "main.gti"
            valid_source = source.read_text()
            source.write_text('import "api.gti"; int main() { return identity(1, 2); }\n')
            rejected = subprocess.run([str(compiler), "check", "app", "--release", "--color=never"],
                                      cwd=consumer, env=env, capture_output=True, text=True, timeout=120)
            (output / (prefix + "private-overload.log")).write_text(rejected.stdout + rejected.stderr)
            assert rejected.returncode != 0 and "identity" in rejected.stderr, "private overload leaked"
            source.write_text(valid_source)
            for operation in ("[[discard]] divide(84, 0)", "divide_and_discard(84, 0)"):
                source.write_text('''import "api.gti";
class Marker {
public:
    Marker() {}
    ~Marker() { std::println("consumer cleanup"); }
};
int main() {
    Marker local{};
    OPERATION;
    return 0;
}
'''.replace("OPERATION", operation))
                name = prefix + ("void-failure" if "divide_and_discard" in operation else "result-failure")
                done = run(consumer, ["run", "app", "--release"], name, 70,
                           "runtime error[GTI-R0002]: division_by_zero in integer_division")
                assert done.stdout.count("consumer cleanup") == 1, f"{name}: cleanup count; {done.stdout}"
                assert "library.gti:" in done.stderr, f"{name}: lost producer provenance"
            source.write_text('''import "api.gti";
class Marker {
public:
    Marker() {}
    ~Marker() {
        std::println("secondary cleanup");
        divide_and_discard(84, 0);
        std::println("unreachable cleanup");
    }
};
int main() {
    Marker local{};
    [[discard]] divide(84, 0);
    return 0;
}
''')
            name = prefix + "secondary-failure-during-consumer-cleanup"
            done = run(consumer, ["run", "app", "--release"], name, 70,
                       f"library.gti:{secondary_line}: runtime error[GTI-R0014]: "
                       "failure_during_cleanup; primary [GTI-R0002] at "
                       f"library.gti:{primary_line}; secondary [GTI-R0002]")
            assert done.stdout.count("secondary cleanup") == 1, f"{name}: duplicate cleanup"
            assert "unreachable cleanup" not in done.stdout, f"{name}: continued after emergency"
            assert (dependencies / "primitives.gtilib").read_bytes() == original
            assert not (consumer / "api.gti").exists()
    print(output / "evidence.json")


if __name__ == "__main__":
    main()
