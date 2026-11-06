#!/usr/bin/env python3

"""Throwaway-repository controls for scripts/check_stdlib_surface_review.py."""

import json
import os
import pathlib
import subprocess
import sys
import tempfile


MANIFEST = "tests/stdlib_surface_manifest.json"


def run(command, cwd, expected=0):
    result = subprocess.run(
        command, cwd=cwd, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
        text=True, check=False,
    )
    if result.returncode != expected:
        raise AssertionError(
            f"expected exit {expected}, got {result.returncode}: "
            f"{' '.join(command)}\nstdout:\n{result.stdout}\nstderr:\n{result.stderr}"
        )
    return result


class Repository:
    def __init__(self, root: pathlib.Path, checker: str):
        self.root = root
        self.checker = checker
        run(["git", "init", "--quiet", "-b", "main"], root)
        run(["git", "config", "user.name", "GTI Test"], root)
        run(["git", "config", "user.email", "gti-test@example.invalid"], root)

    def write(self, relative, text):
        path = self.root / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text, encoding="utf-8")

    def entries(self, entries):
        self.write(MANIFEST, json.dumps({"schema": "gti-stdlib-surface-v1",
                                         "cases": [], "entries": entries}, indent=2) + "\n")

    def commit(self, message):
        run(["git", "add", "-A"], self.root)
        run(["git", "commit", "--quiet", "--allow-empty", "-m", message], self.root)
        return self.head()

    def head(self):
        return run(["git", "rev-parse", "HEAD"], self.root).stdout.strip()

    def branch(self, name, start):
        run(["git", "checkout", "--quiet", "-B", name, start], self.root)

    def check(self, base, head, expected):
        return run([sys.executable, self.checker, base, head], self.root, expected)


def entry(entry_id, source, operations):
    return {"id": entry_id, "source": source, "status": "scaffold",
            "operations": operations, "evidence": ["synthetic"], "reason": "synthetic"}


BASE_ENTRIES = [
    entry("alpha.scaffold", "stdlib/std/alpha.gti", ["std::alpha"]),
    entry("beta.scaffold", "stdlib/std/beta.gti", ["std::beta"]),
    entry("prelude.scaffold", "stdlib/prelude.gti", ["print"]),
]


def expect_unreviewed(result, *sources):
    for source in sources:
        if f"  {source}\n" not in result.stderr:
            raise AssertionError(f"failure does not name {source}:\n{result.stderr}")
    for remedy in ("add, change or remove at least one entry", "Stdlib-Surface: unchanged",
                   "Stdlib-Surface-Reason:", "contiguous trailer block"):
        if remedy not in result.stderr:
            raise AssertionError(f"failure omits remedy {remedy!r}:\n{result.stderr}")


def main():
    if len(sys.argv) != 2:
        raise SystemExit("usage: stdlib_surface_review_test.py check_stdlib_surface_review.py")
    checker = str(pathlib.Path(sys.argv[1]).resolve())
    with tempfile.TemporaryDirectory(prefix="gti-surface-review-") as directory:
        repo = Repository(pathlib.Path(directory), checker)
        repo.write("README.md", "baseline\n")
        repo.write("stdlib/prelude.gti", "// prelude\n")
        repo.write("stdlib/std/alpha.gti", "// alpha\n")
        repo.write("stdlib/std/beta.gti", "// beta\n")
        repo.write("stdlib/README.md", "library\n")
        repo.write("stdlib/tests/src/main.gti", "// package test\n")
        repo.entries(BASE_ENTRIES)
        baseline = repo.commit("baseline")

        def fresh(name):
            repo.branch(name, baseline)

        # Changes outside the shipped census need no review.
        fresh("outside")
        repo.write("README.md", "docs\n")
        repo.write("stdlib/README.md", "library docs\n")
        repo.write("stdlib/tests/src/main.gti", "// package test edit\n")
        outside = repo.commit("non-surface edits")
        result = repo.check(baseline, outside, 0)
        assert "No shipped stdlib source" in result.stdout, result.stdout

        # An unreviewed source edit fails and names the source and both remedies.
        fresh("unreviewed")
        repo.write("stdlib/std/alpha.gti", "// alpha edit\n")
        repo.write("stdlib/prelude.gti", "// prelude edit\n")
        unreviewed = repo.commit("edit alpha and prelude")
        expect_unreviewed(repo.check(baseline, unreviewed, 1),
                          "stdlib/std/alpha.gti", "stdlib/prelude.gti")
        # CI's --advisory mode reports the same finding as warnings and passes.
        local = {key: value for key, value in os.environ.items() if key != "GITHUB_ACTIONS"}
        advisory = subprocess.run([sys.executable, checker, baseline, unreviewed, "--advisory"],
                                  cwd=repo.root, capture_output=True, text=True, check=True, env=local)
        assert not advisory.stderr, advisory.stderr
        for needle in ("advisory: stdlib/std/alpha.gti", "advisory: stdlib/prelude.gti",
                       "Stdlib-Surface: unchanged"):
            assert needle in advisory.stdout, advisory.stdout
        annotated = subprocess.run([sys.executable, checker, baseline, unreviewed, "--advisory"],
                                   cwd=repo.root, capture_output=True, text=True, check=True,
                                   env={**os.environ, "GITHUB_ACTIONS": "true"})
        assert "::warning title=Stdlib surface review::stdlib/std/alpha.gti" in annotated.stdout, annotated.stdout
        broken = subprocess.run([sys.executable, checker, "no-such-revision", unreviewed, "--advisory"],
                                cwd=repo.root, capture_output=True, text=True, check=True, env=local)
        assert broken.stdout.startswith("advisory: stdlib surface review check could not run"), broken.stdout

        # Changing an entry for that source reviews it, even in a later commit;
        # a review of a different source does not.
        entries = json.loads(json.dumps(BASE_ENTRIES))
        entries[0]["operations"] = ["std::alpha", "std::alpha_more"]
        repo.entries(entries)
        partial = repo.commit("review alpha only")
        result = repo.check(baseline, partial, 1)
        expect_unreviewed(result, "stdlib/prelude.gti")
        assert "  stdlib/std/alpha.gti\n" not in result.stderr, result.stderr
        entries[2]["evidence"] = ["synthetic", "prelude reviewed"]
        repo.entries(entries)
        reviewed = repo.commit("review prelude")
        result = repo.check(baseline, reviewed, 0)
        assert "covers every changed stdlib source" in result.stdout, result.stdout

        # A review must be net over the range: a reverted entry change is none.
        repo.entries(BASE_ENTRIES)
        reverted = repo.commit("revert the reviews")
        expect_unreviewed(repo.check(baseline, reverted, 1),
                          "stdlib/std/alpha.gti", "stdlib/prelude.gti")

        # Removing an entry that named the source also reviews it.
        fresh("removed-entry")
        repo.write("stdlib/std/beta.gti", "// beta edit\n")
        repo.entries([BASE_ENTRIES[0], BASE_ENTRIES[2],
                      entry("beta.split", "stdlib/std/beta.gti", ["std::beta"])])
        repo.check(baseline, repo.commit("reclassify beta"), 0)

        # The declaration trailer passes with a reason in the final block.
        fresh("trailer")
        repo.write("stdlib/std/beta.gti", "// beta comment only\n")
        trailer = repo.commit(
            "Reword a beta comment\n\nNo declaration changes.\n\n"
            "Stdlib-Surface: unchanged\n"
            "Stdlib-Surface-Reason: comment-only edit\n"
            "Release-Impact: internal\n"
            "Release-Reason: comment-only edit\n")
        result = repo.check(baseline, trailer, 0)
        assert "declares the stdlib surface unchanged" in result.stdout, result.stdout
        assert "comment-only edit" in result.stdout, result.stdout

        # A declaration in an earlier commit of the range still covers it.
        repo.write("stdlib/std/beta.gti", "// beta comment again\n")
        repo.check(baseline, repo.commit("another edit"), 0)

        # Without a reason, or with a reason outside the final trailer block,
        # or with another value, the declaration does not count.
        for name, message in (
            ("no-reason", "Edit beta\n\nStdlib-Surface: unchanged\n"
                          "Release-Impact: internal\n"),
            ("empty-reason", "Edit beta\n\nStdlib-Surface: unchanged\n"
                             "Stdlib-Surface-Reason:\n"),
            ("split-block", "Edit beta\n\nStdlib-Surface-Reason: comment-only edit\n\n"
                            "Stdlib-Surface: unchanged\n"),
            ("other-value", "Edit beta\n\nStdlib-Surface: changed\n"
                            "Stdlib-Surface-Reason: comment-only edit\n"),
            ("body-text", "Edit beta\n\nStdlib-Surface: unchanged\n"
                          "Stdlib-Surface-Reason: comment-only edit\n\n"
                          "A closing paragraph that is not a trailer block.\n"),
            ("two-reasons", "Edit beta\n\nStdlib-Surface: unchanged\n"
                            "Stdlib-Surface-Reason: one\nStdlib-Surface-Reason: two\n"),
        ):
            fresh(name)
            repo.write("stdlib/std/beta.gti", f"// beta {name}\n")
            expect_unreviewed(repo.check(baseline, repo.commit(message), 1),
                              "stdlib/std/beta.gti")

        # An added source with a new entry passes; without one it fails.
        fresh("added")
        repo.write("stdlib/std/detail/gamma.gti", "// gamma\n")
        added = repo.commit("add gamma")
        expect_unreviewed(repo.check(baseline, added, 1), "stdlib/std/detail/gamma.gti")
        repo.entries(BASE_ENTRIES + [
            entry("gamma.scaffold", "stdlib/std/detail/gamma.gti", ["std::gamma"])])
        repo.check(baseline, repo.commit("classify gamma"), 0)

        # A deletion is left to the manifest's structural census.
        fresh("deleted")
        (repo.root / "stdlib/std/beta.gti").unlink()
        repo.check(baseline, repo.commit("delete beta"), 0)

        # The range starts where head forked: an unreviewed edit that main
        # gained afterwards is not charged to this change, and a base that has
        # advanced past the fork does not hide this change's own edit.
        fresh("main-advanced")
        repo.write("stdlib/std/alpha.gti", "// alpha on main\n")
        advanced = repo.commit("main edits alpha")
        repo.branch("feature", baseline)
        repo.write("README.md", "feature docs\n")
        feature = repo.commit("feature docs")
        repo.check(advanced, feature, 0)
        repo.write("stdlib/std/beta.gti", "// beta on feature\n")
        feature = repo.commit("feature edits beta")
        result = repo.check(advanced, feature, 1)
        expect_unreviewed(result, "stdlib/std/beta.gti")
        assert "  stdlib/std/alpha.gti\n" not in result.stderr, result.stderr

        # A merge into the branch is judged by the net range like any commit.
        run(["git", "merge", "--quiet", "--no-edit", advanced], repo.root)
        expect_unreviewed(repo.check(advanced, repo.head(), 1), "stdlib/std/beta.gti")

        # An unknown revision is a usage error, not a pass.
        run([sys.executable, checker, "no-such-revision", feature], repo.root, 2)

    print("stdlib surface review check rejected unreviewed, undeclared and "
          "malformed-trailer changes and accepted entry reviews and declarations")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
