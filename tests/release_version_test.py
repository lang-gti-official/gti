#!/usr/bin/env python3

import pathlib
import subprocess
import sys
import tempfile


def run(command, cwd, expected=0):
    result = subprocess.run(
        command,
        cwd=cwd,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        check=False,
    )
    if result.returncode != expected:
        raise AssertionError(
            f"expected exit {expected}, got {result.returncode}: "
            f"{' '.join(command)}\nstdout:\n{result.stdout}\nstderr:\n{result.stderr}"
        )
    return result


def commit(root, message):
    run(["git", "add", "."], root)
    run(["git", "commit", "-m", message], root)
    return run(["git", "rev-parse", "HEAD"], root).stdout.strip()


def main():
    if len(sys.argv) != 2:
        raise SystemExit("usage: release_version_test.py check_release_version.py")

    checker = str(pathlib.Path(sys.argv[1]).resolve())
    with tempfile.TemporaryDirectory(prefix="gti-release-version-") as directory:
        root = pathlib.Path(directory)
        run(["git", "init", "--quiet"], root)
        run(["git", "config", "user.name", "GTI Test"], root)
        run(["git", "config", "user.email", "gti-test@example.invalid"], root)

        (root / "include" / "gti").mkdir(parents=True)
        (root / "VERSION").write_text("1.0.0\n", encoding="utf-8")
        (root / "README.md").write_text("baseline\n", encoding="utf-8")
        (root / "include" / "gti" / "frontend.h").write_text(
            "// baseline\n", encoding="utf-8"
        )
        baseline = commit(root, "baseline")

        (root / "README.md").write_text("documentation only\n", encoding="utf-8")
        documentation = commit(root, "documentation")
        run([sys.executable, checker, baseline, documentation], root)

        (root / "include" / "gti" / "frontend.h").write_text(
            "// shipped change\n", encoding="utf-8"
        )
        unversioned = commit(root, "unversioned tool change")
        failure = run(
            [sys.executable, checker, documentation, unversioned], root, expected=1
        )
        if "without a changelog fragment" not in failure.stderr:
            raise AssertionError("missing actionable release-version diagnostic")

        (root / "VERSION").write_text("1.1.0\n", encoding="utf-8")
        released = commit(root, "release tool change\n\nRelease-Cut: 1.1.0")
        run([sys.executable, checker, documentation, released], root)

        header = root / "include" / "gti" / "frontend.h"
        internal_message = (
            "refactor internal representation\n\nRelease-Impact: internal\n"
            "Release-Reason: No change to source semantics or public library APIs."
        )
        header.write_text("// internal refactor\n", encoding="utf-8")
        internal = commit(root, internal_message)
        run([sys.executable, checker, released, internal], root)
        (root / "README.md").write_text("document the refactor\n", encoding="utf-8")
        documented = commit(root, "documentation after internal refactor")
        run([sys.executable, checker, released, documented], root)

        previous = documented
        for index, message in enumerate((
            "missing reason\n\nRelease-Impact: internal",
            "empty reason\n\nRelease-Impact: internal\nRelease-Reason:",
            "wrong impact\n\nRelease-Impact: public\nRelease-Reason: Changed API.",
            "duplicate impact\n\nRelease-Impact: internal\nRelease-Impact: internal\n"
            "Release-Reason: Refactor.",
            "duplicate reason\n\nRelease-Impact: internal\n"
            "Release-Reason: First.\nRelease-Reason: Second.",
            "not trailers\n\nRelease-Impact: internal\nRelease-Reason: Refactor.\n\n"
            "These lines are prose, not the final Git trailer block.",
        )):
            header.write_text(f"// unclassified change {index}\n", encoding="utf-8")
            current = commit(root, message)
            run([sys.executable, checker, previous, current], root, expected=1)
            previous = current

        header.write_text("// later internal refactor\n", encoding="utf-8")
        later = commit(root, internal_message)
        run([sys.executable, checker, previous, later], root)
        run([sys.executable, checker, documented, later], root, expected=1)

        (header.parent / "quoted-é-header.h").write_text(
            "// public declaration\n", encoding="utf-8"
        )
        unusual_path = commit(root, "new header with a Git-quoted path")
        run([sys.executable, checker, later, unusual_path], root, expected=1)

        # Deletion and moves outside release-sensitive roots remain visible.
        header.rename(root / "saved-frontend.txt")
        moved = commit(root, "remove public header from the package")
        run([sys.executable, checker, unusual_path, moved], root, expected=1)
        header.write_text("// restored header\n", encoding="utf-8")
        restored = commit(root, internal_message)
        header.unlink()
        deleted = commit(root, "delete public header")
        run([sys.executable, checker, restored, deleted], root, expected=1)

        (root / "VERSION").write_text("1.0.9\n", encoding="utf-8")
        decreased = commit(root, internal_message)
        run([sys.executable, checker, deleted, decreased], root, expected=1)
        (root / "VERSION").write_text("1.1.0\n", encoding="utf-8")
        churn = commit(root, internal_message)
        # Same final version does not exempt a downgrade-and-restore sequence.
        run([sys.executable, checker, restored, churn], root, expected=1)
        (root / "VERSION").write_text("1.1.1\n", encoding="utf-8")
        final = commit(root, "release accumulated user-visible changes\n\nRelease-Cut: 1.1.1")
        run([sys.executable, checker, churn, final], root)
        # An uncut downgrade-and-restore inside the range is still refused.
        run([sys.executable, checker, documented, final], root, expected=1)

        # A merge delivers its branch commits, each judged on its own; the
        # merge commit itself needs a declaration only for what it introduces
        # beyond every parent. A hosted pull-request merge or an upstream
        # reconciliation merge introduces nothing and carries no trailers.
        run(["git", "checkout", "-b", "internal-side", final], root)
        header.write_text("// side branch internal refactor\n", encoding="utf-8")
        commit(root, internal_message)
        run(["git", "checkout", "-b", "merge-check", final], root)
        run(["git", "merge", "--no-ff", "internal-side", "-m", "merge internal work"], root)
        merged = run(["git", "rev-parse", "HEAD"], root).stdout.strip()
        run([sys.executable, checker, final, merged], root)
        # The merge cannot launder an undeclared branch commit.
        run(["git", "checkout", "-b", "undeclared-side", final], root)
        (header.parent / "other.h").write_text("// undeclared\n", encoding="utf-8")
        commit(root, "undeclared side change")
        run(["git", "checkout", "-b", "undeclared-merge", final], root)
        run(["git", "merge", "--no-ff", "undeclared-side", "-m", "merge undeclared work"], root)
        undeclared_merge = run(["git", "rev-parse", "HEAD"], root).stdout.strip()
        run([sys.executable, checker, final, undeclared_merge], root, expected=1)
        # Reconciling main into a branch, as the task workflow does, is judged
        # the same way whichever side is the first parent.
        run(["git", "checkout", "internal-side"], root)
        run(["git", "merge", "--no-ff", "undeclared-merge", "-m", "reconcile main"], root)
        reconciled = run(["git", "rev-parse", "HEAD"], root).stdout.strip()
        run([sys.executable, checker, undeclared_merge, reconciled], root)
        # Both sides touching the same shipped file in different places still
        # merges automatically and introduces nothing of the merge's own.
        shared = header.parent / "shared.h"
        body = [f"// line {index}\n" for index in range(8)]
        run(["git", "checkout", "-b", "same-file-base", final], root)
        shared.write_text("".join(body), encoding="utf-8")
        commit(root, internal_message)
        run(["git", "checkout", "-b", "same-file-left", "same-file-base"], root)
        shared.write_text("// top\n" + "".join(body[1:]), encoding="utf-8")
        commit(root, internal_message)
        run(["git", "checkout", "-b", "same-file-right", "same-file-base"], root)
        shared.write_text("".join(body[:-1]) + "// bottom\n", encoding="utf-8")
        commit(root, internal_message)
        run(["git", "merge", "--no-ff", "same-file-left", "-m", "merge same file"], root)
        same_file = run(["git", "rev-parse", "HEAD"], root).stdout.strip()
        run([sys.executable, checker, final, same_file], root)
        # Conflict resolution is the merge commit's own shipped content and
        # needs its own declaration.
        run(["git", "checkout", "-b", "conflict-left", final], root)
        header.write_text("// left resolution\n", encoding="utf-8")
        commit(root, internal_message)
        run(["git", "checkout", "-b", "conflict-right", final], root)
        header.write_text("// right resolution\n", encoding="utf-8")
        commit(root, internal_message)
        run(["git", "merge", "--no-ff", "conflict-left", "-m", "resolve"], root, expected=1)
        header.write_text("// resolved by hand\n", encoding="utf-8")
        run(["git", "add", "."], root)
        run(["git", "commit", "-m", "merge with hand resolution"], root)
        resolved = run(["git", "rev-parse", "HEAD"], root).stdout.strip()
        run([sys.executable, checker, final, resolved], root, expected=1)
        run(["git", "commit", "--amend", "-m", internal_message], root)
        annotated_merge = run(["git", "rev-parse", "HEAD"], root).stdout.strip()
        run([sys.executable, checker, final, annotated_merge], root)

        # A changelog fragment describes a shipped change without touching
        # VERSION; the next release cut consumes the fragments.
        run(["git", "checkout", "-b", "fragments", annotated_merge], root)
        changes = root / "changes"
        changes.mkdir()
        header.write_text("// described by a fragment\n", encoding="utf-8")
        (changes / "header-fix.fix.md").write_text("Headers no longer drift.\n", encoding="utf-8")
        described = commit(root, "fix the header")
        accepted = run([sys.executable, checker, annotated_merge, described], root)
        if "changes/header-fix.fix.md" not in accepted.stdout:
            raise AssertionError("fragment acceptance must name the fragment")
        for name, text in (("Header.md", "text"), ("header.minor.md", "text"),
                           ("empty.feature.md", "")):
            run(["git", "checkout", "-q", "-B", "bad-fragment", described], root)
            header.write_text(f"// {name}\n", encoding="utf-8")
            (changes / name).write_text(text, encoding="utf-8")
            bad = commit(root, "bad fragment " + name)
            run([sys.executable, checker, described, bad], root, expected=1)
        run(["git", "checkout", "-q", "-B", "cuts", described], root)
        (root / "VERSION").write_text("1.2.0\n", encoding="utf-8")
        uncut = commit(root, "bump without a cut")
        failure = run([sys.executable, checker, described, uncut], root, expected=1)
        if "Release-Cut" not in failure.stderr:
            raise AssertionError("an uncut VERSION change must point at the release cut")
        run(["git", "checkout", "-q", "-B", "cuts", described], root)
        (root / "VERSION").write_text("1.2.0\n", encoding="utf-8")
        wrong = commit(root, "cut\n\nRelease-Cut: 1.3.0")
        run([sys.executable, checker, described, wrong], root, expected=1)
        run(["git", "checkout", "-q", "-B", "cuts", described], root)
        (root / "VERSION").write_text("1.2.0\n", encoding="utf-8")
        (changes / "header-fix.fix.md").unlink()
        cut = commit(root, "Release 1.2.0\n\nRelease-Cut: 1.2.0")
        run([sys.executable, checker, described, cut], root)

        # Reject unrelated revision comparisons rather than silently omitting
        # changes through a range that never reaches the base.
        run([sys.executable, checker, resolved, annotated_merge], root, expected=2)


if __name__ == "__main__":
    main()
