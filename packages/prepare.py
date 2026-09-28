#!/usr/bin/env python3
"""Prepare pinned native sources for the GTI binding packages (Python 3.10+)."""

from __future__ import annotations

import argparse
import hashlib
import io
import json
import os
from pathlib import Path, PurePosixPath
import platform
import shlex
import shutil
import stat
import subprocess
import sys
import tarfile
import tempfile
import urllib.request
import zipfile

ROOT = Path(__file__).resolve().parent
PACKAGES = ("glfw", "glad", "sqlite3", "cimgui", "zlib", "libpng", "assimp",
            "libgit2")
GENERATOR_REQUIREMENTS = ("Jinja2==3.1.6", "MarkupSafe==3.0.3")


def run(command, **kwargs):
    print("+ " + shlex.join(map(str, command)), flush=True)
    subprocess.run(list(map(str, command)), check=True, **kwargs)


def digest(data):
    return hashlib.sha256(data).hexdigest()


def contained(root, relative):
    parts = PurePosixPath(relative)
    if parts.is_absolute() or not parts.parts or any(
        part in ("..", ".git") or "\\" in part or ":" in part for part in parts.parts
    ):
        raise ValueError(f"unsafe relative path: {relative}")
    current = root
    if current.is_symlink():
        raise ValueError(f"refusing symlink: {current}")
    for part in parts.parts:
        current = current / part
        if current.is_symlink():
            raise ValueError(f"refusing symlink: {current}")
    return current


def archive_files(data, zipped=False, exclude_directories=()):
    """Validate the complete archive before creating any source files."""
    entries = []
    def excluded(name):
        # Validate even excluded names before choosing not to read their data.
        contained(Path("/archive-check"), name)
        parts = PurePosixPath(name).parts
        return len(parts) > 1 and parts[1] in exclude_directories
    if zipped:
        with zipfile.ZipFile(io.BytesIO(data)) as archive:
            for item in archive.infolist():
                if excluded(item.filename):
                    continue
                if stat.S_ISLNK(item.external_attr >> 16):
                    raise ValueError(f"archive symlink: {item.filename}")
                entries.append((item.filename, None if item.is_dir() else archive.read(item)))
    else:
        with tarfile.open(fileobj=io.BytesIO(data), mode="r:gz") as archive:
            for item in archive.getmembers():
                if excluded(item.name):
                    continue
                if not (item.isdir() or item.isfile()):
                    raise ValueError(f"archive special entry: {item.name}")
                entries.append((item.name, None if item.isdir() else archive.extractfile(item).read()))
    roots = set()
    files = {}
    folded = set()
    for name, contents in entries:
        contained(Path("/archive-check"), name)
        parts = PurePosixPath(name).parts
        roots.add(parts[0])
        if len(parts) == 1:
            if contents is not None:
                raise ValueError("expected a single top-level archive directory")
            continue
        relative = str(PurePosixPath(*parts[1:]))
        if contents is not None:
            if relative.casefold() in folded:
                raise ValueError(f"duplicate archive file: {relative}")
            folded.add(relative.casefold())
            files[relative] = contents
    if len(roots) != 1 or not files:
        raise ValueError("expected one nonempty archive root")
    return files


def acquire(entry, cache, offline):
    archive = contained(cache, entry["sha256"])
    limit = entry.get("max-download-bytes", 64 * 1024 * 1024)
    if archive.exists():
        if archive.stat().st_size > limit:
            raise ValueError("cached archive exceeds the locked size limit")
        data = archive.read_bytes()
    else:
        if offline:
            raise ValueError(f"offline: missing cached archive for {entry['url']}")
        print(f"Downloading {entry['url']}", flush=True)
        with urllib.request.urlopen(entry["url"], timeout=60) as response:
            data = response.read(limit + 1)
        if len(data) > limit:
            raise ValueError("archive exceeds the locked size limit")
    if digest(data) != entry["sha256"]:
        raise ValueError(f"checksum mismatch for {entry['url']}")
    files = archive_files(data, entry["url"].endswith(".zip"), entry.get("exclude-directories", ()))
    destination = contained(ROOT, entry["destination"])
    # Preflight all existing paths before writing; never overwrite edited sources.
    for relative, content in files.items():
        path = contained(destination, relative)
        if path.exists() and (not path.is_file() or path.read_bytes() != content):
            raise ValueError(f"modified upstream source; refusing to overwrite: {path}")
    if not archive.exists():
        with archive.open("xb") as output:
            output.write(data)
    for relative, content in files.items():
        path = contained(destination, relative)
        if not path.exists():
            path.parent.mkdir(parents=True, exist_ok=True)
            with path.open("xb") as output:
                output.write(content)
    print(f"Verified {destination} ({len(files)} files)", flush=True)


def prepare_glad(cache, offline):
    output = contained(ROOT, "glad/native/generated")
    marker = output / "generation.json"
    identity = {"api": "gl:core=3.3", "extensions": [], "loader": True,
                "requirements": list(GENERATOR_REQUIREMENTS),
                "source": json.loads((ROOT / "upstream-lock.json").read_text())["glad"]["sha256"]}
    if output.exists():
        if not marker.is_file():
            raise ValueError(f"incomplete GLAD generation at {output}; inspect and move it aside")
        record = json.loads(marker.read_text())
        if record.get("identity") != identity:
            raise ValueError("GLAD generation options changed; move the old generated directory aside")
        for name, checksum in record["files"].items():
            if digest(contained(output, name).read_bytes()) != checksum:
                raise ValueError(f"modified generated GLAD file: {name}")
        return
    environment = contained(cache, "glad-python")
    python = environment / "bin/python"
    if not python.exists():
        if offline:
            raise ValueError("offline: GLAD's isolated Python environment has not been prepared")
        run([sys.executable, "-m", "venv", environment])
    # Exact versions, isolated from the user's Python environment. No upgrade of pip.
    run([python, "-m", "pip", "install", *( ["--no-index"] if offline else []),
         *GENERATOR_REQUIREMENTS])
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".glad-", dir=output.parent) as directory:
        staged = Path(directory) / "generated"
        run([python, "-m", "glad", "--reproducible", "--api", "gl:core=3.3",
             "--extensions", "", "--out-path", staged, "c", "--loader"],
            cwd=ROOT / "glad/vendor/glad")
        files = {str(path.relative_to(staged)): digest(path.read_bytes())
                 for path in sorted(staged.rglob("*")) if path.is_file()}
        (staged / "generation.json").write_text(json.dumps({"identity": identity, "files": files}, indent=2) + "\n")
        staged.rename(output)


def prepare_consumer_zlib(consumer):
    canonical_zlib_source = ROOT / "zlib/vendor/zlib"
    zlib_source = contained(ROOT, f"{consumer}/vendor/zlib")
    if not zlib_source.exists():
        shutil.copytree(canonical_zlib_source, zlib_source)


def generate_bindings(name, gti):
    namespace = {"sqlite3": "sqlite", "glad": "gl", "cimgui": "imgui",
                 "zlib": "zlib", "libpng": "png", "assimp": "assimp",
                 "libgit2": "git"}[name]
    package = ROOT / name
    destination = contained(package, f"src/{name}.gti")
    marker = contained(package, "src/.bindings.json")
    if destination.exists():
        if not marker.is_file() or json.loads(marker.read_text()).get("sha256") != digest(destination.read_bytes()):
            raise ValueError(f"modified or untracked binding; refusing to overwrite: {destination}")
    command = [gti, "bindgen", package / "native/bindings.h", "--namespace", namespace]
    if name == "sqlite3":
        command += ["-I", package / "vendor/sqlite3"]
        command += ["-D", "SQLITE_THREADSAFE=1", "-D", "SQLITE_OMIT_LOAD_EXTENSION=1"]
        for pattern in ("sqlite3", "sqlite3_*", "SQLITE_*", "gti_sqlite_*"):
            command += ["--allow", pattern]
        # Header declarations also describe optional builds and other platforms.
        # Do not advertise symbols absent from this package's native configuration.
        for pattern in ("sqlite3_carray_*", "sqlite3_column_database_name*",
                        "sqlite3_column_origin_name*", "sqlite3_column_table_name*",
                        "sqlite3_enable_load_extension", "sqlite3_mutex_held",
                        "sqlite3_mutex_notheld", "sqlite3_rtree_*", "sqlite3_snapshot_*",
                        "sqlite3_stmt_scanstatus*", "sqlite3_win32_*"):
            command += ["--deny", pattern]
    elif name == "assimp":
        command += ["-I", package / "vendor/assimp/include", "-I", package / "native/generated"]
        # Upstream headers mark themselves as system headers. Bind the explicit
        # C adapter instead, without overriding bindgen's provenance boundary.
        for pattern in ("aiScene", "aiNode", "aiMesh", "GtiAssimp*", "gti_assimp_*"):
            command += ["--allow", pattern]
    elif name == "libgit2":
        command += ["-I", package / "vendor/libgit2/include"]
        for pattern in ("GtiGit*", "gti_git_*"):
            command += ["--allow", pattern]
    elif name == "zlib":
        command += ["-I", package / "vendor/zlib"]
        for pattern in ("zlibVersion", "zError", "compress*", "uncompress*",
                        "crc32*", "adler32*", "Z_*"):
            command += ["--allow", pattern]
    # Bindgen aliases are intentionally private to a package. Generate into the
    # public unit itself so a direct package include owns visible declarations.
    with tempfile.TemporaryDirectory(prefix=".bindings-", dir=package / "src") as directory:
        staged = Path(directory) / f"{name}.gti"
        run([*command, "-o", staged])
        checksum = digest(staged.read_bytes())
        os.replace(staged, destination)
        staged_marker = Path(directory) / "marker.json"
        staged_marker.write_text(json.dumps({"sha256": checksum}) + "\n")
        os.replace(staged_marker, marker)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("packages", nargs="*", choices=PACKAGES)
    parser.add_argument("--jobs", type=int, default=os.cpu_count() or 1,
                        help="native library build workers (default: logical CPU count)")
    parser.add_argument("--offline", action="store_true")
    parser.add_argument("--gti", default="gti", help="GTI compiler used to generate public bindings")
    args = parser.parse_args()
    if args.jobs < 1:
        parser.error("--jobs must be positive")
    os_name = {"Darwin": "macos", "Linux": "linux"}.get(platform.system())
    arch = {"arm64": "arm64", "aarch64": "arm64", "x86_64": "x86_64"}.get(platform.machine())
    if not os_name or not arch:
        parser.error("currently supports native 64-bit macOS and Linux hosts only")
    selected = args.packages or list(PACKAGES)
    if any(name in selected for name in ("libpng", "assimp", "libgit2")) and "zlib" not in selected:
        selected.append("zlib")
    gti = shutil.which(args.gti)
    if any(name != "glfw" for name in selected) and not gti:
        parser.error("GTI compiler not found; pass --gti /path/to/gti")
    cache = contained(ROOT, ".cache")
    cache.mkdir(exist_ok=True)
    lock = json.loads((ROOT / "upstream-lock.json").read_text())
    for name in dict.fromkeys(selected + (["imgui"] if "cimgui" in selected else [])):
        acquire(lock[name], cache, args.offline)
    if "libpng" in selected:
        # Native groups are isolated: copy the exact dependency headers into the
        # PNG group, but compile/link the zlib implementation only in its package.
        headers = {"pnglibconf.h": ROOT / "libpng/vendor/libpng/scripts/pnglibconf.h.prebuilt",
                   "zlib.h": ROOT / "zlib/vendor/zlib/zlib.h",
                   "zconf.h": ROOT / "zlib/vendor/zlib/zconf.h"}
        for name, source in headers.items():
            target = contained(ROOT, f"libpng/native/generated/{name}")
            if target.exists() and target.read_bytes() != source.read_bytes():
                raise ValueError(f"modified generated header: {target}")
            if not target.exists():
                target.parent.mkdir(parents=True, exist_ok=True)
                with target.open("xb") as output:
                    output.write(source.read_bytes())
    if "glad" in selected:
        prepare_glad(cache, args.offline)
    if "assimp" in selected:
        prepare_consumer_zlib("assimp")
    if "libgit2" in selected:
        prepare_consumer_zlib("libgit2")
    for name in selected:
        if name != "glfw":
            generate_bindings(name, gti)
    print("Ready. See packages/README.md for consumer and smoke-test commands.")


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        raise SystemExit(str(error)) from error
