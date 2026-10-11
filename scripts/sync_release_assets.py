#!/usr/bin/env python3
"""Prune a GitHub release to the asset set the publication step just uploaded."""

import argparse
import json
import os
import pathlib
import sys
import urllib.error
import urllib.parse
import urllib.request
from collections.abc import Callable
from typing import Any


ApiRequest = Callable[[str, str], Any]
DEFAULT_API_URL = "https://api.github.com"


def repository_parts(repository: str) -> tuple[str, str]:
    parts = repository.split("/")
    if len(parts) != 2 or not all(parts):
        raise ValueError("repository must be owner/name")
    owner, name = (urllib.parse.quote(part, safe="") for part in parts)
    return owner, name


def release_path(repository: str, tag: str) -> str:
    owner, name = repository_parts(repository)
    if not tag:
        raise ValueError("tag must not be empty")
    return f"/repos/{owner}/{name}/releases/tags/{urllib.parse.quote(tag, safe='')}"


def asset_path(repository: str, asset_id: int) -> str:
    # GitHub addresses a release asset by its own id, not through its release.
    owner, name = repository_parts(repository)
    return f"/repos/{owner}/{name}/releases/assets/{asset_id}"


def make_api_request(api_url: str, token: str) -> ApiRequest:
    base = api_url.rstrip("/")
    parsed = urllib.parse.urlparse(base)
    if parsed.scheme not in {"http", "https"} or not parsed.netloc:
        raise ValueError("GitHub API URL must be an absolute HTTP(S) URL")
    if not token:
        raise ValueError("GITHUB_TOKEN is required")

    def request(method: str, path: str) -> Any:
        operation = urllib.request.Request(
            base + path,
            method=method,
            headers={
                "Accept": "application/vnd.github+json",
                "Authorization": f"Bearer {token}",
                "X-GitHub-Api-Version": "2022-11-28",
                "User-Agent": "gti-release-asset-sync",
            },
        )
        try:
            with urllib.request.urlopen(operation, timeout=30) as response:
                payload = response.read()
        except urllib.error.HTTPError as error:
            detail = error.read().decode("utf-8", errors="replace").strip()
            raise RuntimeError(
                f"GitHub API {method} {path} failed with HTTP {error.code}: "
                f"{detail or error.reason}"
            ) from error
        except urllib.error.URLError as error:
            raise RuntimeError(
                f"GitHub API {method} {path} failed: {error.reason}"
            ) from error
        if not payload:
            return None
        try:
            return json.loads(payload)
        except json.JSONDecodeError as error:
            raise RuntimeError(
                f"GitHub API {method} {path} returned invalid JSON"
            ) from error

    return request


def sync_assets(
    repository: str,
    tag: str,
    asset_directory: pathlib.Path,
    request: ApiRequest,
) -> list[str]:
    if not asset_directory.is_dir():
        raise ValueError(f"asset directory does not exist: {asset_directory}")
    expected = sorted(path.name for path in asset_directory.iterdir() if path.is_file())
    if not expected:
        raise ValueError(f"asset directory is empty: {asset_directory}")

    release = request("GET", release_path(repository, tag))
    if not isinstance(release, dict) or not isinstance(release.get("id"), int):
        raise RuntimeError("GitHub release response has no integer release id")
    assets = release.get("assets")
    if not isinstance(assets, list):
        raise RuntimeError("GitHub release response has no asset inventory")

    expected_names = set(expected)
    stale: list[tuple[str, int]] = []
    for asset in assets:
        if not isinstance(asset, dict):
            raise RuntimeError("GitHub release asset entry is not an object")
        name = asset.get("name")
        identifier = asset.get("id")
        if not isinstance(name, str) or not isinstance(identifier, int):
            raise RuntimeError("GitHub release asset has no stable name and id")
        if name not in expected_names:
            stale.append((name, identifier))

    stale.sort()
    for _, identifier in stale:
        request("DELETE", asset_path(repository, identifier))
    return [name for name, _ in stale]


def main() -> int:
    parser = argparse.ArgumentParser(
        description=(
            "Prune a GitHub release to the asset set already uploaded by the "
            "publication step."
        )
    )
    parser.add_argument("--api-url", default=DEFAULT_API_URL)
    parser.add_argument("--repository", required=True)
    parser.add_argument("--tag", required=True)
    parser.add_argument("--asset-directory", required=True, type=pathlib.Path)
    arguments = parser.parse_args()

    try:
        request = make_api_request(
            arguments.api_url,
            os.environ.get("GITHUB_TOKEN") or os.environ.get("GH_TOKEN", ""),
        )
        stale = sync_assets(
            arguments.repository,
            arguments.tag,
            arguments.asset_directory,
            request,
        )
    except (OSError, RuntimeError, ValueError) as error:
        print(f"GitHub release asset synchronization failed: {error}", file=sys.stderr)
        return 1

    if stale:
        print("Removed stale GitHub release assets: " + ", ".join(stale))
    else:
        print("GitHub release assets already match the current build.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
