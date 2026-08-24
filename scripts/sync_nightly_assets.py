#!/usr/bin/env python3

import argparse
import pathlib
import subprocess
import sys
from collections.abc import Callable


GhInvocation = Callable[[list[str]], str]


def run_gh(arguments: list[str]) -> str:
    result = subprocess.run(
        ["gh", *arguments],
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    if result.returncode != 0:
        detail = result.stderr.strip() or result.stdout.strip() or "gh failed"
        raise RuntimeError(detail)
    return result.stdout


def sync_assets(
    tag: str,
    asset_directory: pathlib.Path,
    invoke: GhInvocation = run_gh,
) -> list[str]:
    if not tag:
        raise ValueError("release tag must not be empty")
    if not asset_directory.is_dir():
        raise ValueError(f"asset directory does not exist: {asset_directory}")

    assets = sorted(path for path in asset_directory.iterdir() if path.is_file())
    if not assets:
        raise ValueError(f"asset directory is empty: {asset_directory}")

    # Replace current names before pruning. If publication is interrupted, the
    # rolling release retains at least the newest archives that were uploaded.
    invoke(
        [
            "release",
            "upload",
            tag,
            *(str(path) for path in assets),
            "--clobber",
        ]
    )

    published = invoke(
        [
            "release",
            "view",
            tag,
            "--json",
            "assets",
            "--jq",
            ".assets[].name",
        ]
    )
    expected = {path.name for path in assets}
    stale = sorted(
        {
            name
            for line in published.splitlines()
            if (name := line.strip()) and name not in expected
        }
    )
    for name in stale:
        invoke(["release", "delete-asset", tag, name, "--yes"])
    return stale


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Synchronize an existing rolling release's asset set."
    )
    parser.add_argument("tag", help="existing rolling release tag")
    parser.add_argument("asset_directory", type=pathlib.Path)
    arguments = parser.parse_args()

    try:
        stale = sync_assets(arguments.tag, arguments.asset_directory)
    except (OSError, RuntimeError, ValueError) as error:
        print(f"nightly asset synchronization failed: {error}", file=sys.stderr)
        return 1

    if stale:
        print("Removed stale nightly assets: " + ", ".join(stale))
    else:
        print("Nightly assets already match the current build.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
