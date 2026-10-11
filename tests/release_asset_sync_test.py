#!/usr/bin/env python3

import importlib.util
import pathlib
import sys
import tempfile


def load_module(path: pathlib.Path):
    specification = importlib.util.spec_from_file_location("sync_release_assets", path)
    if specification is None or specification.loader is None:
        raise AssertionError(f"could not load {path}")
    module = importlib.util.module_from_spec(specification)
    specification.loader.exec_module(module)
    return module


def expect(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def main() -> None:
    if len(sys.argv) != 2:
        raise SystemExit("usage: release_asset_sync_test.py sync_release_assets.py")

    module = load_module(pathlib.Path(sys.argv[1]).resolve())
    with tempfile.TemporaryDirectory(prefix="gti-release-assets-") as directory:
        root = pathlib.Path(directory)
        names = [
            "BUILD_ID",
            "gti-nightly-darwin-arm64.tar.gz",
            "gti-nightly-darwin-arm64.tar.gz.sha256",
            "gti-nightly-linux-x64.tar.gz",
            "gti-nightly-linux-x64.tar.gz.sha256",
        ]
        for name in names:
            (root / name).write_text(name, encoding="utf-8")
        (root / "ignored-directory").mkdir()

        published = [
            {"id": 3, "name": names[4]},
            {"id": 1, "name": names[0]},
            {"id": 2, "name": names[3]},
            {"id": 6, "name": names[1]},
            {"id": 7, "name": names[2]},
            {"id": 5, "name": "gti-nightly-darwin-x64.tar.gz.sha256"},
            {"id": 4, "name": "gti-nightly-darwin-x64.tar.gz"},
        ]
        calls: list[tuple[str, str]] = []

        def request(method: str, path: str):
            calls.append((method, path))
            if method == "GET":
                return {"id": 91, "assets": published}
            return None

        stale = module.sync_assets("lang-gti-official/gti", "nightly", root, request)
        expect(
            calls
            == [
                ("GET", "/repos/lang-gti-official/gti/releases/tags/nightly"),
                # GitHub deletes an asset by its own id, in deterministic name order.
                ("DELETE", "/repos/lang-gti-official/gti/releases/assets/4"),
                ("DELETE", "/repos/lang-gti-official/gti/releases/assets/5"),
            ],
            f"only stale GitHub assets may be removed, in deterministic order: {calls}",
        )
        expect(
            stale == ["gti-nightly-darwin-x64.tar.gz", "gti-nightly-darwin-x64.tar.gz.sha256"],
            "retired assets were not selected exactly",
        )

        empty = root / "empty"
        empty.mkdir()
        empty_calls: list[tuple[str, str]] = []
        try:
            module.sync_assets(
                "lang-gti-official/gti",
                "nightly",
                empty,
                lambda method, path: empty_calls.append((method, path)),
            )
        except ValueError as error:
            expect("is empty" in str(error), "empty-set failure is not actionable")
        else:
            raise AssertionError("an empty asset set must fail closed")
        expect(not empty_calls, "empty input must fail before calling GitHub")

        for repository, tag in (("missing-owner", "nightly"), ("owner/name", "")):
            try:
                module.release_path(repository, tag)
            except ValueError:
                pass
            else:
                raise AssertionError("malformed release identity must fail closed")

        try:
            module.make_api_request("https://api.github.com", "")
        except ValueError as error:
            expect("GITHUB_TOKEN" in str(error), "missing token is not actionable")
        else:
            raise AssertionError("a missing token must fail closed")
        try:
            module.make_api_request("api.github.com", "token")
        except ValueError:
            pass
        else:
            raise AssertionError("a relative API URL must fail closed")


if __name__ == "__main__":
    main()
