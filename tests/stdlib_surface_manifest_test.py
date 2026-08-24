#!/usr/bin/env python3

"""Injected-negative checks for the stdlib surface census."""

from __future__ import annotations

import copy
import pathlib
import sys

import stdlib_surface_test as surface


def expect_failure(
    manifest: dict[str, object], repository: pathlib.Path, fragment: str
) -> None:
    try:
        surface.validate_manifest(manifest, repository)
    except surface.ManifestError as error:
        if fragment not in str(error):
            raise AssertionError(
                f"wanted validation failure containing {fragment!r}, got {error!r}"
            ) from error
        return
    raise AssertionError(f"manifest unexpectedly passed injected failure {fragment!r}")


def main() -> int:
    if len(sys.argv) != 2:
        raise SystemExit("usage: stdlib_surface_manifest_test.py <repository>")
    repository = pathlib.Path(sys.argv[1]).resolve()
    original = surface.load_manifest(
        repository / "tests" / "stdlib_surface_manifest.json"
    )
    surface.validate_manifest(original, repository)

    missing_source = copy.deepcopy(original)
    missing_source["source_fingerprints"].pop("stdlib/std/utility.gti")
    expect_failure(missing_source, repository, "stdlib source census differs")

    stale_source = copy.deepcopy(original)
    stale_source["source_fingerprints"]["stdlib/std/utility.gti"] = "0" * 64
    expect_failure(stale_source, repository, "changed without a surface review")

    missing_evidence = copy.deepcopy(original)
    executable = next(
        entry
        for entry in missing_evidence["entries"]
        if entry["status"] == "executable"
    )
    executable["case_ids"] = []
    expect_failure(missing_evidence, repository, "has no run case")

    wrong_case_kind = copy.deepcopy(original)
    diagnostic_id = next(
        case["id"]
        for case in wrong_case_kind["cases"]
        if case["mode"] == "diagnostic"
    )
    executable = next(
        entry
        for entry in wrong_case_kind["entries"]
        if entry["status"] == "executable"
    )
    executable["case_ids"] = [diagnostic_id]
    expect_failure(wrong_case_kind, repository, "references a non-run case")

    print("stdlib surface census rejected missing, stale, and mismatched evidence")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
