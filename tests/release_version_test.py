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
        if "without advancing VERSION" not in failure.stderr:
            raise AssertionError("missing actionable release-version diagnostic")

        (root / "VERSION").write_text("1.1.0\n", encoding="utf-8")
        released = commit(root, "release tool change")
        run([sys.executable, checker, documentation, released], root)


if __name__ == "__main__":
    main()
