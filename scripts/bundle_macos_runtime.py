#!/usr/bin/env python3
"""Make staged macOS command-line tools independent of movable host dylibs."""

from __future__ import annotations

import argparse
import json
import os
import pathlib
import re
import shutil
import subprocess


def command(arguments):
    return subprocess.run([str(arg) for arg in arguments], check=True,
                          capture_output=True, text=True).stdout


def install_id(path):
    lines = command(["otool", "-D", path]).splitlines()[1:]
    return lines[0].strip() if lines else None


def dependencies(path):
    identifier = install_id(path)
    result = []
    for line in command(["otool", "-L", path]).splitlines()[1:]:
        name = line.strip().split(" (compatibility version ", 1)[0]
        if name and name != identifier:
            result.append(name)
    return result


def rpaths(path):
    text = command(["otool", "-l", path])
    return re.findall(r"cmd LC_RPATH\s+cmdsize \d+\s+path (.*?) \(offset \d+\)",
                      text)


def system_library(name):
    return name.startswith(("/usr/lib/", "/System/Library/"))


def expand_path(name, owner, executable):
    for prefix, base in (("@loader_path", owner.parent),
                         ("@executable_path", executable.parent)):
        if name == prefix or name.startswith(prefix + "/"):
            return base / name[len(prefix):].lstrip("/")
    return pathlib.Path(name)


def retained_homebrew_candidates(name):
    """Resolve an exact missing install name, never alias a different ABI."""
    path = pathlib.Path(name)
    parts = path.parts
    for index in range(len(parts) - 2, 0, -1):
        if parts[index] != "opt":
            continue
        cellar = pathlib.Path(*parts[:index]) / "Cellar" / parts[index + 1]
        suffix = pathlib.Path(*parts[index + 2:])
        if cellar.is_dir():
            return sorted({candidate.resolve() for candidate in cellar.glob("*/" + str(suffix))
                           if candidate.is_file()})
    return []


def resolve_library(name, owner, executable, overrides, search_dirs,
                    inherited_rpaths):
    if name in overrides:
        return overrides[name]
    if name.startswith("@rpath/"):
        suffix = name[len("@rpath/"):]
        # Explicitly selected library siblings precede ambient Homebrew paths.
        candidates = [directory / suffix for directory in search_dirs]
        candidates.extend(directory / suffix for directory in
                          [*(expand_path(value, owner, executable)
                             for value in rpaths(owner)), *inherited_rpaths])
    else:
        candidates = [expand_path(name, owner, executable)]
    for candidate in candidates:
        if candidate.is_file():
            return candidate.resolve()
    retained = retained_homebrew_candidates(name)
    if len(retained) == 1:
        return retained[0]
    detail = " (multiple retained candidates)" if retained else ""
    raise ValueError(f"cannot resolve runtime dependency {name!r} of {owner}{detail}; "
                     "select a matching supported LLVM installation and rebuild")


def stage_destination(source, stage):
    if source.is_relative_to(stage):
        return source
    return stage / "lib/gti/host-runtime" / source.name


def license_files(source):
    # Library directories and their package prefix, not an unrelated ancestor
    # repository/home-directory license. Missing attribution fails closed.
    for directory in list(source.parents)[:3]:
        files = [directory / name for name in
                 ("LICENSE.TXT", "LICENSE.txt", "LICENSE", "COPYING")]
        found = [file for file in files if file.is_file()]
        if found:
            return found
    raise ValueError(f"runtime library {source} has no discoverable license; "
                     "provide a redistributable dependency installation")


def plan_bundle(stage, selected_libraries):
    stage = stage.resolve()
    roots = [stage / "bin/gti"]
    if (stage / "bin/gti_lsp").exists():
        roots.append(stage / "bin/gti_lsp")
    if not all(root.is_file() and not root.is_symlink() for root in roots):
        raise ValueError("staging must contain regular tool executable files")
    selected = [path.resolve(strict=True) for path in selected_libraries]
    overrides = {}
    search_dirs = []
    for library in selected:
        identifier = install_id(library)
        if not identifier:
            raise ValueError(f"selected runtime library is not a dylib: {library}")
        staged = stage / "lib/gti/llvm" / library.name
        source = staged.resolve() if staged.is_file() else library
        overrides[identifier] = source
        search_dirs.append(library.parent)
    libraries = {}
    edges = {}
    pending = [(root, root, []) for root in roots]
    visited = set()
    while pending:
        owner, executable, inherited = pending.pop()
        if owner in visited:
            continue
        visited.add(owner)
        edges[owner] = []
        next_rpaths = [*(expand_path(value, owner, executable)
                        for value in rpaths(owner)), *inherited]
        for name in dependencies(owner):
            if system_library(name):
                continue
            source = resolve_library(name, owner, executable, overrides,
                                     search_dirs, inherited)
            if system_library(str(source)):
                continue
            destination = stage_destination(source, stage)
            if destination in libraries.values() and source not in libraries:
                raise ValueError(f"different runtime libraries share {destination.name}; "
                                 "select one coherent dependency installation")
            libraries[source] = destination
            edges[owner].append((name, source))
            pending.append((source, executable, next_rpaths))
    licenses = {source: license_files(source) for source in libraries
                if not source.is_relative_to(stage)}
    for path in [*roots, *libraries.values(),
                 stage / "share/gti/host-runtime.json",
                 *(stage / "share/licenses/gti/host-runtime" / destination.name
                   for destination in libraries.values()),
                 *(stage / "share/licenses/gti/host-runtime" / libraries[source].name / file.name
                   for source, files in licenses.items() for file in files)]:
        if path.is_symlink() or not path.resolve().is_relative_to(stage):
            raise ValueError(f"staging destination escapes or aliases another file: {path}")
    return stage, roots, libraries, edges, licenses


def bundle(stage, selected_libraries):
    stage, roots, libraries, edges, licenses = plan_bundle(stage, selected_libraries)
    # Resolve the entire closure and licenses before modifying staged files.
    for source, destination in libraries.items():
        if source != destination:
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(source, destination)
            directory = stage / "share/licenses/gti/host-runtime" / destination.name
            directory.mkdir(parents=True, exist_ok=True)
            for license in licenses[source]:
                shutil.copy2(license, directory / license.name)
        destination.chmod(destination.stat().st_mode | 0o200)
    for source, source_edges in edges.items():
        destination = libraries.get(source, source)
        arguments = ["install_name_tool"]
        if source in libraries:
            arguments.extend(["-id", "@rpath/" + destination.name])
        for old, target in source_edges:
            relative = os.path.relpath(libraries[target], destination.parent)
            arguments.extend(["-change", old, "@loader_path/" + relative])
        if len(arguments) > 1:
            command([*arguments, destination])
    for destination in [*libraries.values(), *roots]:
        command(["codesign", "--force", "--sign", "-", destination])
    for destination in [*libraries.values(), *roots]:
        for name in dependencies(destination):
            if system_library(name):
                continue
            if not name.startswith("@loader_path/"):
                raise ValueError(f"staged runtime still has a host dependency: {name}")
            target = expand_path(name, destination, roots[0]).resolve(strict=True)
            if not target.is_relative_to(stage):
                raise ValueError(f"staged runtime dependency escapes the toolchain: {name}")
    manifest = stage / "share/gti/host-runtime.json"
    manifest.parent.mkdir(parents=True, exist_ok=True)
    manifest.write_text(json.dumps({"schema": 1, "platform": "darwin",
                                   "libraries": sorted(str(path.relative_to(stage))
                                                       for path in libraries.values())},
                                  indent=2) + "\n")
    return len(libraries)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--stage", type=pathlib.Path, required=True)
    parser.add_argument("--library", type=pathlib.Path, action="append", default=[])
    args = parser.parse_args()
    if os.uname().sysname != "Darwin":
        parser.error("runtime dylib bundling requires macOS")
    stage = args.stage.absolute()
    if stage.is_symlink() or stage.resolve() == pathlib.Path("/"):
        parser.error("--stage must be a real staged toolchain directory")
    count = bundle(stage, args.library)
    print(f"Bundled and verified {count} relocatable macOS runtime libraries")


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        raise SystemExit(f"macOS runtime bundling failed: {error}") from error
