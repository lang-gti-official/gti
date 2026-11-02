#!/usr/bin/env python3
"""Stage the checksum-pinned Neovim used by hosted editor validation."""
import hashlib
import os
from pathlib import Path
import platform
import shutil
import subprocess
import tarfile
import urllib.request

VERSION = "v0.11.5"
ASSETS = {
    ("Linux", "x86_64"): ("nvim-linux-x86_64", "b2f91117be5b5ea39edd7297156dc2a4a8df4add6c95a90809a8df19e7ab6f52"),
    ("Darwin", "arm64"): ("nvim-macos-arm64", "79143d3b408f7034f90b7cf59af2276de09ef8a4c2f1a28e4c99581b249d3107"),
}


def stage(root, env_path, system=None, machine=None):
    identity = (system or platform.system(), machine or platform.machine())
    if identity not in ASSETS:
        raise ValueError(f"No reviewed editor asset for {identity}")
    asset, digest = ASSETS[identity]
    root.mkdir(parents=True, exist_ok=True)
    archive = root / (asset + ".tar.gz")
    url = f"https://github.com/neovim/neovim/releases/download/{VERSION}/{asset}.tar.gz"
    with urllib.request.urlopen(url, timeout=60) as source, archive.open("wb") as target:
        shutil.copyfileobj(source, target)
    if hashlib.sha256(archive.read_bytes()).hexdigest() != digest:
        raise ValueError("Neovim checksum mismatch")
    with tarfile.open(archive) as stream:
        stream.extractall(root, filter="data")
    executable = root / asset / "bin/nvim"
    subprocess.run([str(executable), "--version"], check=True)
    with env_path.open("a") as target:
        target.write(str(executable.parent) + "\n")
    return executable


if __name__ == "__main__":
    stage(Path(os.environ["RUNNER_TEMP"]) / "gti-ci-editor", Path(os.environ["GITHUB_PATH"]))
