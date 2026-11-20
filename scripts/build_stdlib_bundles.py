#!/usr/bin/env python3
"""Build target-specific stdlib objects with the staged compiler and runtime."""
import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import tomllib


def catalog(source):
    directory = source / "cmake/stdlib-bundles"
    with (directory / "gti.toml").open("rb") as manifest:
        targets = tomllib.load(manifest)["targets"]
    facades = {path.stem for path in (directory / "facades").glob("*.gti")}
    if not targets or targets.keys() != facades:
        raise ValueError("each installed library target needs exactly one matching source facade")
    modules = {}
    for name, target in targets.items():
        entry = Path(target["root"]).relative_to("stdlib")
        if target["kind"] != "gti-library" or ".." in entry.parts or entry in modules.values():
            raise ValueError(f"invalid or duplicate installed library entry for {name}")
        modules[name] = entry
    return modules


def write_registry(source, modules, output):
    # The compiler embeds only this reviewed toolchain catalog. Consumer
    # projects cannot supply module overlays or require a runtime manifest read.
    lines = []
    for name, entry in modules.items():
        facade = (source / "cmake/stdlib-bundles/facades" / f"{name}.gti").read_text()
        fields = ", ".join(json.dumps(value, ensure_ascii=False)
                           for value in (name, entry.as_posix(), facade))
        lines.append("    {" + fields + "},")
    contents = "\n".join(lines) + "\n"
    if not output.exists() or output.read_text() != contents:
        output.parent.mkdir(parents=True, exist_ok=True)
        output.write_text(contents)


def build(stage, source, modules):
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
        outputs = []
        for profile in ("dev", "release"):
            for name in modules:
                command = [str(stage / "bin/gti"), "build", name, "--offline"]
                if profile == "release":
                    command.append("--release")
                subprocess.run(command, cwd=package, env=env, check=True)
                bundle, = (package / "build/gti" / profile).glob(f"*/{name}.gtilib")
                relative = Path("precompiled-stdlib") / bundle.parent.name / profile / bundle.name
                candidate = package / relative
                candidate.parent.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(bundle, candidate)
                outputs.append((candidate, data / relative))

        # Exercise normal named import and re-export from a separate package.
        # Only the scratch copy of the implementation is removed. The source
        # companion still provides constexpr and the ordinary toolchain
        # retains sources for editors and unsupported configurations.
        for entry in modules.values():
            (package / "stdlib" / entry).unlink()
        consumer = package / "consumer"
        shutil.copytree(source / "tests/project/standard_library_bundle", consumer)
        compiler = str(stage / "bin/gti")
        for profile in ([], ["--release"]):
            for command in ("check", "run"):
                subprocess.run([compiler, command, "--offline", *profile],
                               cwd=consumer, env=env, check=True)
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
    args = parser.parse_args()
    source = Path(__file__).resolve().parents[1]
    modules = catalog(source)
    if args.stage:
        build(args.stage, source, modules)
    elif args.write_registry:
        write_registry(source, modules, args.write_registry)
    else:
        print(";".join(modules))
