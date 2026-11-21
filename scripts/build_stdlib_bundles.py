#!/usr/bin/env python3
"""Build target-specific stdlib objects with the staged compiler and runtime."""
import argparse
import hashlib
import json
from dataclasses import dataclass
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import tomllib


@dataclass(frozen=True)
class InstalledModule:
    entry: Path
    facade: Path | None  # Absent when the bundle carries its module recipe.


def catalog(source):
    directory = source / "cmake/stdlib-bundles"
    with (directory / "gti.toml").open("rb") as manifest:
        targets = tomllib.load(manifest)["targets"]
    facades = {path.stem for path in (directory / "facades").glob("*.gti")}
    native = {name for name, target in targets.items() if target.get("functions")}
    if not targets or not facades.issubset(native):
        raise ValueError("a standalone facade requires a native library target")
    modules = {}
    for name, target in targets.items():
        entry = Path(target["root"]).relative_to("stdlib")
        if target["kind"] != "gti-library" or ".." in entry.parts or entry in {m.entry for m in modules.values()}:
            raise ValueError(f"invalid or duplicate installed library entry for {name}")
        modules[name] = InstalledModule(entry, directory / "facades" / f"{name}.gti" if name in facades else None)
    return modules


def write_registry(modules, output):
    # The compiler embeds only this reviewed toolchain catalog. Consumer
    # projects cannot supply module overlays or require a runtime manifest read.
    lines = []
    for name, module in modules.items():
        facade = module.facade.read_text() if module.facade else ""
        if module.facade and not facade.strip():
            raise ValueError(f"empty native facade for {name}")
        fields = ", ".join(json.dumps(value, ensure_ascii=False)
                           for value in (name, module.entry.as_posix(), facade))
        lines.append("    {" + fields + "},")
    contents = "\n".join(lines) + "\n"
    if not output.exists() or output.read_text() != contents:
        output.parent.mkdir(parents=True, exist_ok=True)
        output.write_text(contents)


def build(stage, source):
    modules = catalog(source)
    stage = stage.resolve()
    data = stage / "share/gti"
    # A library producer's entry must be in its package. Use a private package
    # containing the actual installed sources, with their normal toolchain
    # provenance. Never make the application's package root compiler-trusted.
    with tempfile.TemporaryDirectory(prefix="gti-stdlib-") as directory:
        package = Path(directory)
        shutil.copyfile(source / "cmake/stdlib-bundles/gti.toml", package / "gti.toml")
        shutil.copytree(data / "stdlib", package / "stdlib")
        env = dict(os.environ, GTI_STDLIB_PATH=str(package / "stdlib"),
                   GTI_RUNTIME_LIBRARY=str(stage / "lib/libgti_runtime.a"),
                   GTI_RUNTIME_INCLUDE=str(stage / "include"))
        # Each manifest target has its own output. Let the project builder
        # schedule them, then collect artifacts only after the whole profile succeeds.
        cpus = len(os.sched_getaffinity(0)) if hasattr(os, "sched_getaffinity") else os.cpu_count() or 1
        jobs = min(4, cpus)
        outputs = []
        for profile in ("dev", "release"):
            subprocess.run([str(stage / "bin/gti"), "build", "--all", "--offline",
                            "--profile", profile, "--jobs", str(jobs)],
                           cwd=package, env=env, check=True)
            for name in modules:
                bundle, = (package / "build/gti" / profile).glob(f"*/{name}.gtilib")
                relative = Path("precompiled-stdlib") / bundle.parent.name / profile / bundle.name
                candidate = package / relative
                candidate.parent.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(bundle, candidate)
                outputs.append((candidate, data / relative))
                if modules[name].facade is None:
                    receipt = candidate.with_suffix(".gtilib.sha256")
                    receipt.write_text(hashlib.sha256(candidate.read_bytes()).hexdigest() + "\n")
                    outputs.append((receipt, (data / relative).with_suffix(".gtilib.sha256")))

        # Exercise normal named import and re-export from a separate package.
        # Only the scratch copy of the implementation is removed. The source
        # companion still provides constexpr and the ordinary toolchain
        # retains sources for editors and unsupported configurations.
        for entry in {m.entry for m in modules.values()}:
            (package / "stdlib" / entry).unlink()
        consumer = package / "consumer"
        shutil.copytree(source / "tests/project/standard_library_bundle", consumer)
        compiler = str(stage / "bin/gti")
        for profile in ([], ["--release"]):
            for command in ("check", "run"):
                subprocess.run([compiler, command, "--offline", *profile],
                               cwd=consumer, env=env, check=True)
        # Compile and execute a consumer-owned substitution after its producer
        # root has been removed from the toolchain used by the application.
        cases = (consumer / "array_consumer.gti",
                 source / "tests/ui/stdlib/vector/constexpr_range_for_views.gti",
                 source / "tests/ui/callables/borrowing_captures/borrowing_captures_19.gti")
        for case in cases:
            shutil.copyfile(case, consumer / "src/main.gti")
            for profile in ([], ["--release"]):
                subprocess.run([compiler, "run", "--offline", *profile], cwd=consumer, env=env, check=True)
        for bundle, destination in outputs:
            destination.parent.mkdir(parents=True, exist_ok=True)
            with tempfile.NamedTemporaryFile(dir=destination.parent, delete=False) as temp:
                try:
                    with bundle.open("rb") as incoming:
                        shutil.copyfileobj(incoming, temp)
                    temp.close()
                    os.chmod(temp.name, 0o644)
                    os.replace(temp.name, destination)
                finally:
                    Path(temp.name).unlink(missing_ok=True)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    action = parser.add_mutually_exclusive_group(required=True)
    action.add_argument("--stage", type=Path)
    action.add_argument("--write-registry", type=Path)
    action.add_argument("--list-targets", action="store_true")
    action.add_argument("--list-template-targets", action="store_true")
    args = parser.parse_args()
    source = Path(__file__).resolve().parents[1]
    if args.stage:
        build(args.stage, source)
    else:
        modules = catalog(source)
        if args.write_registry:
            write_registry(modules, args.write_registry)
        elif args.list_template_targets:
            print(";".join(name for name, module in modules.items() if module.facade is None))
        else:
            print(";".join(modules))
