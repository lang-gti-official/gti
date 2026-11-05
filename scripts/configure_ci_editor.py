#!/usr/bin/env python3
"""Stage the checksum-pinned Neovim used by hosted editor validation."""
import hashlib
import os
from pathlib import Path
import platform
import shutil
import subprocess
import tarfile
import time
import urllib.error
import urllib.request

VERSION = "v0.11.5"
ASSETS = {
    ("Linux", "x86_64"): ("nvim-linux-x86_64", "b2f91117be5b5ea39edd7297156dc2a4a8df4add6c95a90809a8df19e7ab6f52"),
    ("Darwin", "arm64"): ("nvim-macos-arm64", "79143d3b408f7034f90b7cf59af2276de09ef8a4c2f1a28e4c99581b249d3107"),
}


def download(url, archive, attempts=6, delay=5.0, sleep=time.sleep):
    """Fetch url into archive, retrying transient network and server failures.

    A hosted runner's DNS lookup or connection occasionally fails for a few
    seconds (full validation 71 lost a whole macOS shard that way), and
    GitHub's release CDN returns bursts of 5xx (run 41995172449). Waits double
    from delay up to a minute, about two minutes in all; a client error such
    as 404 fails at once. The checksum below still rejects any wrong bytes.
    """
    for attempt in range(1, attempts + 1):
        try:
            with urllib.request.urlopen(url, timeout=60) as source, archive.open("wb") as target:
                shutil.copyfileobj(source, target)
            return
        except urllib.error.HTTPError as error:
            if error.code < 500 and error.code != 429 or attempt == attempts:
                raise
            failure = error
        except (urllib.error.URLError, TimeoutError, ConnectionError) as error:
            if attempt == attempts:
                raise
            failure = error
        print(f"editor download attempt {attempt} failed ({failure}); retrying", flush=True)
        sleep(min(delay * 2 ** (attempt - 1), 60.0))


def stage(root, env_path, system=None, machine=None):
    identity = (system or platform.system(), machine or platform.machine())
    if identity not in ASSETS:
        raise ValueError(f"No reviewed editor asset for {identity}")
    asset, digest = ASSETS[identity]
    root.mkdir(parents=True, exist_ok=True)
    archive = root / (asset + ".tar.gz")
    url = f"https://github.com/neovim/neovim/releases/download/{VERSION}/{asset}.tar.gz"
    download(url, archive)
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
