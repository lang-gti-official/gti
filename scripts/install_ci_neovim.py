#!/usr/bin/env python3
"""Install a pinned, checksummed editor into the disposable CI workspace."""
import hashlib
import os
from pathlib import Path
import platform
import tarfile
import urllib.request

ASSETS = {
    ("Linux", "x86_64"): ("nvim-linux-x86_64", "a74740047e73b2b380d63a474282814063d10650cd6cc95efa16d1713c7e616c"),
    ("Darwin", "arm64"): ("nvim-macos-arm64", "2de9623a4aa8cedf85c51e33bf8e85e05f6f291b923cd666c04704ccf164e8b7"),
}


def main():
    name, expected = ASSETS[(platform.system(), platform.machine())]
    root = Path(os.environ["RUNNER_TEMP"]) / "gti-ci-neovim"
    root.mkdir(exist_ok=False)
    archive = root / "neovim.tar.gz"
    urllib.request.urlretrieve("https://github.com/neovim/neovim/releases/download/v0.11.4/" + name + ".tar.gz", archive)
    with archive.open("rb") as stream:
        if hashlib.file_digest(stream, "sha256").hexdigest() != expected:
            raise ValueError("Neovim checksum mismatch")
    with tarfile.open(archive) as stream:
        stream.extractall(root, filter="data")
    with open(os.environ["GITHUB_PATH"], "a") as out:
        out.write(str(root / name / "bin") + "\n")


if __name__ == "__main__":
    main()
