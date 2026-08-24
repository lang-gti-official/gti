#!/usr/bin/env python3

import importlib.util
import pathlib
import sys
import tempfile


def load_module(path: pathlib.Path):
    specification = importlib.util.spec_from_file_location(
        "sync_nightly_assets", path
    )
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
        raise SystemExit(
            "usage: nightly_asset_sync_test.py sync_nightly_assets.py"
        )

    module = load_module(pathlib.Path(sys.argv[1]).resolve())
    with tempfile.TemporaryDirectory(prefix="gti-nightly-assets-") as directory:
        root = pathlib.Path(directory)
        names = [
            "BUILD_ID",
            "gti-nightly-darwin-arm64.tar.gz",
            "gti-nightly-darwin-arm64.tar.gz.sha256",
            "gti-nightly-linux-arm64.tar.gz",
            "gti-nightly-linux-arm64.tar.gz.sha256",
            "gti-nightly-linux-x64.tar.gz",
            "gti-nightly-linux-x64.tar.gz.sha256",
        ]
        for name in names:
            (root / name).write_text(name, encoding="utf-8")
        (root / "ignored-directory").mkdir()

        published = [
            *reversed(names),
            "gti-nightly-darwin-x64.tar.gz.sha256",
            "gti-nightly-darwin-x64.tar.gz",
        ]
        calls: list[tuple[str, ...]] = []

        def invoke(arguments: list[str]) -> str:
            calls.append(tuple(arguments))
            if arguments[:2] == ["release", "view"]:
                return "\n".join(published) + "\n"
            return ""

        stale = module.sync_assets("nightly", root, invoke)
        expected_paths = tuple(str(root / name) for name in sorted(names))
        expect(
            calls[0]
            == (
                "release",
                "upload",
                "nightly",
                *expected_paths,
                "--clobber",
            ),
            "current assets must upload before any remote inspection or pruning",
        )
        expect(
            calls[1]
            == (
                "release",
                "view",
                "nightly",
                "--json",
                "assets",
                "--jq",
                ".assets[].name",
            ),
            "remote asset inventory command drifted",
        )
        expect(
            stale
            == [
                "gti-nightly-darwin-x64.tar.gz",
                "gti-nightly-darwin-x64.tar.gz.sha256",
            ],
            "retired Darwin x64 assets were not selected exactly",
        )
        expect(
            calls[2:]
            == [
                (
                    "release",
                    "delete-asset",
                    "nightly",
                    "gti-nightly-darwin-x64.tar.gz",
                    "--yes",
                ),
                (
                    "release",
                    "delete-asset",
                    "nightly",
                    "gti-nightly-darwin-x64.tar.gz.sha256",
                    "--yes",
                ),
            ],
            "only stale assets may be removed, in deterministic order",
        )

        empty = root / "empty"
        empty.mkdir()
        empty_calls: list[list[str]] = []
        try:
            module.sync_assets(
                "nightly", empty, lambda arguments: empty_calls.append(arguments)
            )
        except ValueError as error:
            expect("is empty" in str(error), "empty-set failure is not actionable")
        else:
            raise AssertionError("an empty asset set must fail closed")
        expect(not empty_calls, "empty input must fail before invoking gh")


if __name__ == "__main__":
    main()
