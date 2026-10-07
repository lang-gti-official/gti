#!/usr/bin/env python3
"""Resolve exact source/channel identities without building or mutating Git."""

from __future__ import annotations

import argparse
import pathlib
import re
import subprocess
import sys


SHA = re.compile(r"[0-9a-f]{40}")
VERSION = re.compile(r"[0-9]+\.[0-9]+\.[0-9]+")


def git(*arguments: str, optional: bool = False) -> str:
    result = subprocess.run(["git", *arguments], capture_output=True, text=True,
                            timeout=30, check=False)
    if result.returncode and not optional:
        raise ValueError(result.stderr.strip() or "Git source query failed")
    return result.stdout.strip() if result.returncode == 0 else ""


def checked_sha(value: str) -> str:
    if SHA.fullmatch(value) is None:
        raise ValueError("source identity must be a full lowercase commit SHA")
    if git("rev-parse", "HEAD") != value:
        raise ValueError("checked-out commit does not match the triggering revision")
    return value


def source(sha: str, event: str, before: str) -> dict[str, str]:
    sha = checked_sha(sha)
    version = git("show", f"{sha}:VERSION")
    if VERSION.fullmatch(version) is None:
        raise ValueError("VERSION must contain a semantic version")
    if pathlib.Path("VERSION").read_text(encoding="utf-8").strip() != version:
        raise ValueError("working VERSION differs from the selected source commit")
    stable = event == "workflow_dispatch"
    if event == "push":
        if not before or before == "0" * 40:
            before = git("rev-parse", "--verify", "HEAD^", optional=True)
        elif SHA.fullmatch(before) is None:
            raise ValueError("previous source identity must be a full commit SHA")
        previous = git("show", f"{before}:VERSION") if before else ""
        if previous and VERSION.fullmatch(previous) is None:
            raise ValueError("previous VERSION must contain a semantic version")
        stable = version != previous
        if previous and tuple(map(int, version.split("."))) < tuple(
                map(int, previous.split("."))):
            raise ValueError("VERSION cannot decrease")
    tag = f"v{version}"
    if stable:
        existing = git("rev-parse", "--verify", f"refs/tags/{tag}^{{commit}}",
                       optional=True)
        if existing and existing != sha:
            raise ValueError(f"immutable tag {tag} names {existing}, not {sha}")
    return {"sha": sha, "version": version, "tag": tag,
            "stable": str(stable).lower()}


def admit_nightly(sha: str) -> dict[str, str]:
    sha = checked_sha(sha)
    fields = git("ls-remote", "--exit-code", "origin", "refs/heads/main").split()
    if len(fields) != 2 or SHA.fullmatch(fields[0]) is None or fields[1] != "refs/heads/main":
        raise ValueError("origin did not advertise one exact main commit")
    current = fields[0]
    admitted = current == sha
    print(f"Nightly source {sha}: {'current' if admitted else 'superseded by ' + current}")
    return {"publish": str(admitted).lower()}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("mode", choices=("source", "admit-nightly"))
    parser.add_argument("--sha", required=True)
    parser.add_argument("--event", choices=("push", "workflow_dispatch"), default="push")
    parser.add_argument("--before", default="")
    parser.add_argument("--output", type=pathlib.Path, required=True)
    arguments = parser.parse_args()
    try:
        values = (source(arguments.sha, arguments.event, arguments.before)
                  if arguments.mode == "source" else admit_nightly(arguments.sha))
        # Write runner state only after every identity and immutable-tag check.
        with arguments.output.open("a", encoding="utf-8") as output:
            output.write("".join(f"{name}={value}\n" for name, value in values.items()))
    except (OSError, ValueError, subprocess.TimeoutExpired) as error:
        print(f"release source check failed: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
