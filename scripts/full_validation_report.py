#!/usr/bin/env python3
"""Keep one open `main-broken` issue current when full validation of main fails.

Full validation runs after integration (.github/workflows/full-validation.yml),
so a red result reaches nobody's pull request. This script turns it into one
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

LABEL = "main-broken"
TITLE = "Full validation of main is failing"
SHA = re.compile(r"[0-9a-f]{40}")


def gh(*arguments: str, stdin: str | None = None) -> str:
    result = subprocess.run(["gh", *arguments], capture_output=True, text=True,
                            timeout=120, check=False, input=stdin)
    if result.returncode:
        raise RuntimeError(result.stderr.strip() or "gh failed")
    return result.stdout.strip()


def open_issue() -> dict | None:
    rows = json.loads(gh("issue", "list", "--label", LABEL, "--state", "open",
                         "--json", "number,title", "--limit", "5") or "[]")
    return next((row for row in rows if row.get("title") == TITLE), None)


def body(run_url: str, before: str, sha: str, status: str) -> str:
    compare = f"{before[:12]}...{sha[:12]}" if SHA.fullmatch(before) and not set(before) == {"0"} else sha[:12]
    return (
        f"Full validation of `main` **{status}** at `{sha}`.\n\n"
        f"- Run: {run_url}\n"
        f"- Commits since the last run: `{compare}`\n\n"
        "The merge queue proves only build and fast checks, so this run is the first time the "
        "complete suite (packaging, every integration shard, the compiler sanitizers) saw these "
        "commits. Until it is green, no channel (nightly, beta, stable) selects this head.\n\n"
        "To resolve: fix forward through an ordinary PR, or revert the offending merge. "
        "This issue is updated on each later failure and closed automatically when main passes.\n"
    )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--run-url", required=True)
    parser.add_argument("--before", default="")
    parser.add_argument("--sha", required=True)
    parser.add_argument("--status", choices=("failed", "passed"), default="failed")
    arguments = parser.parse_args()
    if SHA.fullmatch(arguments.sha) is None:
        print("the head must be a full commit SHA", file=sys.stderr)
        return 1
    if not re.fullmatch(r"https://github\.com/[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+/actions/runs/[0-9]+", arguments.run_url):
        print("the run URL must be a GitHub Actions run", file=sys.stderr)
        return 1
    text = body(arguments.run_url, arguments.before, arguments.sha, arguments.status)
    existing = open_issue()
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
        gh("label", "create", LABEL, "--force", "--color", "B60205",
           "--description", "Full validation of main is failing")
        url = gh("issue", "create", "--title", TITLE, "--label", LABEL, "--body-file", "-", stdin=text)
        print(f"opened {url}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
