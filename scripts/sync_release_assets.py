#!/usr/bin/env python3
"""Prune a Gitea release only after the publication step uploaded its complete asset set."""

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
DEFAULT_API_URL = "https://git.internal.howetotech.dev/api/v1"


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


def asset_path(repository: str, release_id: int, asset_id: int) -> str:
    owner, name = repository_parts(repository)
    return f"/repos/{owner}/{name}/releases/{release_id}/assets/{asset_id}"


class NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        return None


def make_api_request(api_url: str, token: str) -> ApiRequest:
    base = api_url.rstrip("/")
    if base != DEFAULT_API_URL:
        raise ValueError("Gitea API URL must be the canonical HTTPS endpoint")
    if not token:
        raise ValueError("GTI_GITEA_TOKEN or GITEA_TOKEN is required")

    def request(method: str, path: str) -> Any:
        operation = urllib.request.Request(
            base + path,
            method=method,
            headers={
                "Accept": "application/json",
                "Authorization": f"token {token}",
                "User-Agent": "gti-release-asset-sync",
            },
        )
        try:
            with urllib.request.build_opener(NoRedirect).open(operation, timeout=30) as response:
                payload = response.read()
        except urllib.error.HTTPError as error:
            error.close()
            raise RuntimeError(
                f"Gitea API {method} {path} failed with HTTP {error.code}"
            ) from error
        except urllib.error.URLError as error:
            raise RuntimeError(
                f"Gitea API {method} {path} is unavailable"
            ) from error
        if not payload:
            return None
        try:
            return json.loads(payload)
        except json.JSONDecodeError as error:
            raise RuntimeError(
                f"Gitea API {method} {path} returned invalid JSON"
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
        raise RuntimeError("Gitea release response has no integer release id")
    assets = release.get("assets")
    if not isinstance(assets, list):
        raise RuntimeError("Gitea release response has no asset inventory")

    expected_names = set(expected)
    published_names = set()
    stale: list[tuple[str, int]] = []
    for asset in assets:
        if not isinstance(asset, dict):
            raise RuntimeError("Gitea release asset entry is not an object")
        name = asset.get("name")
        identifier = asset.get("id")
        if not isinstance(name, str) or not isinstance(identifier, int):
            raise RuntimeError("Gitea release asset has no stable name and id")
        published_names.add(name)
        if name not in expected_names:
            stale.append((name, identifier))

    if not expected_names.issubset(published_names):
        raise RuntimeError("Current assets have not all been uploaded; refusing to prune the release")
    stale.sort()
    for _, identifier in stale:
        request("DELETE", asset_path(repository, release["id"], identifier))
    return [name for name, _ in stale]


def main() -> int:
    parser = argparse.ArgumentParser(
        description=(
            "Prune a Gitea release to the asset set already uploaded by the "
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
            os.environ.get("GTI_GITEA_TOKEN") or os.environ.get("GITEA_TOKEN", ""),
        )
        stale = sync_assets(
            arguments.repository,
            arguments.tag,
            arguments.asset_directory,
            request,
        )
    except (OSError, RuntimeError, ValueError) as error:
        print(f"Gitea release asset synchronization failed: {error}", file=sys.stderr)
        return 1

    if stale:
        print("Removed stale Gitea release assets: " + ", ".join(stale))
    else:
        print("Gitea release assets already match the current build.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
