#!/usr/bin/env python3

"""Injected-negative checks for the stdlib surface census."""

from __future__ import annotations

import copy
import importlib.util
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


def load_review_check(repository: pathlib.Path):
    path = repository / "scripts" / "check_stdlib_surface_review.py"
    spec = importlib.util.spec_from_file_location("check_stdlib_surface_review", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def main() -> int:
    if len(sys.argv) != 2:
        raise SystemExit("usage: stdlib_surface_manifest_test.py <repository>")
    repository = pathlib.Path(sys.argv[1]).resolve()
    original = surface.load_manifest(
        repository / "tests" / "stdlib_surface_manifest.json"
    )
    surface.validate_manifest(original, repository)

    # The census is the shipped tree: a source no entry classifies fails, and
    # so does an entry naming a source the tree does not ship.
    missing_source = copy.deepcopy(original)
    missing_source["entries"] = [
        entry for entry in missing_source["entries"]
        if entry["source"] != "stdlib/std/utility.gti"
    ]
    expect_failure(missing_source, repository, "stdlib source census differs")
    expect_failure(missing_source, repository, "stdlib/std/utility.gti")

    extra_source = copy.deepcopy(original)
    extra_source["entries"][0]["source"] = "stdlib/std/not_shipped.gti"
    expect_failure(extra_source, repository, "outside the stdlib source census")

    # Review evidence comes from the change's diff, never a stored snapshot.
    retired = copy.deepcopy(original)
    retired["source_fingerprints"] = {}
    expect_failure(retired, repository, "source_fingerprints is retired")

    # The diff-based review check and this census must agree on which files
    # are shipped stdlib sources.
    review = load_review_check(repository)
    census = {
        path.relative_to(repository).as_posix()
        for path in surface.stdlib_sources(repository)
    }
    candidates = {
        path.relative_to(repository).as_posix()
        for path in (repository / "stdlib").rglob("*") if path.is_file()
    }
    judged = {path for path in candidates if review.is_surface_source(path)}
    if judged != census:
        raise AssertionError(
            "check_stdlib_surface_review.py and stdlib_sources disagree: "
            f"review-only={sorted(judged - census)}, census-only={sorted(census - judged)}"
        )

    missing_evidence = copy.deepcopy(original)
    executable = next(
        entry
        for entry in missing_evidence["entries"]
        if entry["status"] == "executable"
    )
    executable["case_ids"] = []
    expect_failure(missing_evidence, repository, "has no run case")

    wrong_case_kind = copy.deepcopy(original)
    executable = next(
        entry
        for entry in wrong_case_kind["entries"]
        if entry["status"] == "executable" and entry["case_ids"]
    )
    diagnostic_id = executable["case_ids"][0]
    diagnostic = next(
        case for case in wrong_case_kind["cases"] if case["id"] == diagnostic_id
    )
    diagnostic["mode"] = "diagnostic"
    diagnostic.pop("optimizations")
    diagnostic["optimization"] = "O0"
    diagnostic["expected"] = {
        "exit_code": 65,
        "stderr_contains": ["synthetic diagnostic evidence"],
    }
    expect_failure(wrong_case_kind, repository, "references a non-run case")

    unbounded_timeout = copy.deepcopy(original)
    unbounded_timeout["cases"][0]["build_timeout_seconds"] = 3600
    unbounded_timeout["cases"][0]["build_timeout_reason"] = "synthetic"
    expect_failure(unbounded_timeout, repository, "to at most 900")

    unexplained_timeout = copy.deepcopy(original)
    unexplained_timeout["cases"][0]["build_timeout_seconds"] = 600
    unexplained_timeout["cases"][0].pop("build_timeout_reason", None)
    expect_failure(unexplained_timeout, repository, "needs a build_timeout_reason")

    print("stdlib surface census rejected missing, extra, retired, and mismatched evidence")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
