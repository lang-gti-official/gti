#!/usr/bin/env python3
"""Keep one open `main-broken` issue current when full validation of main fails.

Full validation runs after integration (.github/workflows/full-validation.yml),
and the sanitizers run daily on the nightly commit (.github/workflows/
sanitizers.yml), so a red result reaches nobody's pull request. Each source
keeps its own issue under the `main-broken` label, told apart by title, so one
workflow's green run never closes the other's finding. This script turns it into one
issue that names the commit range and the run, updating that issue on every
later failure and closing it when main is green again, so a broken main is
never left unnoticed. Everything it writes is derived from GitHub's own event
fields; it never interpolates untrusted text into a shell.
"""

from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys

DEFAULT_LABEL = "main-broken"
DEFAULT_TITLE = "Full validation of main is failing"
SANITIZER_TITLE = "Sanitizers on the nightly commit are failing"
SHA = re.compile(r"[0-9a-f]{40}")


def gh(*arguments: str, stdin: str | None = None) -> str:
    result = subprocess.run(["gh", *arguments], capture_output=True, text=True,
                            timeout=120, check=False, input=stdin)
    if result.returncode:
        raise RuntimeError(result.stderr.strip() or "gh failed")
    return result.stdout.strip()


def open_issue(label: str, title: str) -> dict | None:
    rows = json.loads(gh("issue", "list", "--label", label, "--state", "open",
                         "--json", "number,title", "--limit", "5") or "[]")
    return next((row for row in rows if row.get("title") == title), None)


def body(run_url: str, before: str, sha: str, status: str, label: str,
         title: str = DEFAULT_TITLE) -> str:
    compare = f"{before[:12]}...{sha[:12]}" if SHA.fullmatch(before) and not set(before) == {"0"} else sha[:12]
    if label == DEFAULT_LABEL and title == SANITIZER_TITLE:
        return (
            f"The daily sanitizers **{status}** on the nightly commit `{sha}`.\n\n"
            f"- Run: {run_url}\n\n"
            "The compiler (ASan/UBSan), LLVM-backend (ASan/UBSan) and thread sanitizers run once a day "
            "on the commit the `nightly` tag names, not on every main push. Until they pass, the release "
            "train cuts no beta from this nightly and promotes no beta built on it.\n\n"
            "To resolve: fix forward through an ordinary PR; the next nightly is sanitized the following "
            "day, or dispatch sanitizers.yml with the fixed commit. This issue is updated on each later "
            "failure and closed automatically when the sanitizers pass.\n"
        )
    if label == DEFAULT_LABEL:
        return (
            f"Full validation of `main` **{status}** at `{sha}`.\n\n"
            f"- Run: {run_url}\n"
            f"- Commits since the last run: `{compare}`\n\n"
            "The merge queue proves only build and fast checks, so this run is the first time the "
            "complete suite (packaging and every integration shard) saw these commits. Until it is "
            "green, no channel (nightly, beta, stable) selects this head.\n\n"
            "To resolve: fix forward through an ordinary PR, or revert the offending merge. "
            "This issue is updated on each later failure and closed automatically when main passes.\n"
        )
    return (
        f"The scheduled `{label}` run **{status}** at `{sha}`.\n\n"
        f"- Run: {run_url}\n\n"
        "This run proves the deprecated C++ backend on its own schedule under one profile. "
        "It never blocks the merge queue, full validation or a channel; a failure here is a "
        "backend drift to triage, not a broken main. This issue is updated on each later "
        "failure and closed automatically when the run passes.\n"
    )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--run-url", required=True)
    parser.add_argument("--before", default="")
    parser.add_argument("--sha", required=True)
    parser.add_argument("--status", choices=("failed", "passed"), default="failed")
    parser.add_argument("--label", default=DEFAULT_LABEL)
    parser.add_argument("--title", default=DEFAULT_TITLE)
    arguments = parser.parse_args()
    if not re.fullmatch(r"[a-z][a-z0-9-]{1,40}", arguments.label) or len(arguments.title) > 120:
        print("the label must be a short lowercase slug and the title short", file=sys.stderr)
        return 1
    if SHA.fullmatch(arguments.sha) is None:
        print("the head must be a full commit SHA", file=sys.stderr)
        return 1
    if not re.fullmatch(r"https://github\.com/[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+/actions/runs/[0-9]+", arguments.run_url):
        print("the run URL must be a GitHub Actions run", file=sys.stderr)
        return 1
    text = body(arguments.run_url, arguments.before, arguments.sha, arguments.status, arguments.label,
                arguments.title)
    existing = open_issue(arguments.label, arguments.title)
    if arguments.status == "passed":
        if existing:
            gh("issue", "comment", str(existing["number"]), "--body-file", "-", stdin=text)
            gh("issue", "close", str(existing["number"]), "--reason", "completed")
            print(f"closed #{existing['number']}")
        return 0
    if existing:
        gh("issue", "comment", str(existing["number"]), "--body-file", "-", stdin=text)
        print(f"updated #{existing['number']}")
    else:
        gh("label", "create", arguments.label, "--force", "--color", "B60205",
           "--description", arguments.title)
        url = gh("issue", "create", "--title", arguments.title, "--label", arguments.label,
                 "--body-file", "-", stdin=text)
        print(f"opened {url}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
