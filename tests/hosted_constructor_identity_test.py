#!/usr/bin/env python3
"""Hosted argv and ordinary dependency storage share concrete constructors."""

import pathlib
import shutil
import subprocess
import sys
import tempfile

from lambda_pack_invocation_runtime_test import main as runtime_matrix


def main():
    compiler = pathlib.Path(sys.argv[1]).resolve()
    fixture = pathlib.Path(__file__).parent / "fixtures/hosted_constructor_identity"
    expected = (0, "1\n2\n9\n", "")
    runtime_matrix(names=("hosted_constructor_identity",), cases={
        "hosted_constructor_identity": [((), expected), (("one", "two"), expected)]})
    with tempfile.TemporaryDirectory(prefix="gti-hosted-identity-") as directory:
        root = pathlib.Path(directory) / "project"
        shutil.copytree(fixture, root)
        for profile in ("dev", "release"):
            result = subprocess.run(
                [str(compiler), "run", "app", "--profile", profile,
                 "--progress=never", "--", "one", "two"],
                cwd=root / "consumer", capture_output=True, text=True, timeout=120)
            assert result.returncode == 0, (profile, result.stdout, result.stderr)
            assert result.stdout == expected[1], (profile, result.stdout)
            progress = result.stderr.splitlines()
            assert len(progress) == 3 and progress[0].startswith("Built app [") \
                and progress[1] == "Diagnostics: 0 errors, 0 warnings" \
                and progress[2].startswith("Running "), (profile, result.stderr)
            print(f"hosted constructor package identity: {profile} passed", flush=True)


if __name__ == "__main__":
    main()
