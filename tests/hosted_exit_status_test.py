#!/usr/bin/env python3
"""Portable hosted entry status preserves failures and invocation cleanup."""

import pathlib
import shutil
import subprocess
import sys
import tempfile

from lambda_pack_invocation_runtime_test import main as runtime_matrix
from harness_timeouts import scaled


def main():
    compiler = pathlib.Path(sys.argv[1]).resolve()
    fixtures = pathlib.Path(__file__).parent / "fixtures"
    owned = [
        (("x",) * count, (status, "cleanup\n", ""))
        for count, status in enumerate((0, 1, 255, 1, 1, 1, 1, 1))
    ]
    runtime_matrix(
        names=("hosted_exit_status_owned", "hosted_exit_status_plain",
               "hosted_exit_status_module", "hosted_exit_status_cleanup"),
        cases={"hosted_exit_status_owned": owned,
               "hosted_exit_status_plain": [((), (1, "", ""))],
               "hosted_exit_status_module": [((), (1, "", ""))],
               "hosted_exit_status_cleanup": [((), (1, "cleanup\n", ""))]})
    with tempfile.TemporaryDirectory(prefix="gti-hosted-exit-") as temporary:
        root = pathlib.Path(temporary)
        project = root / "project"
        shutil.copytree(pathlib.Path(__file__).parent / "coverage/projects/"
                        "a02-test-status-truncation", project)
        for profile in ("dev", "release"):
            tested = subprocess.run(
                [str(compiler), "test", "--profile", profile,
                 "--progress=never", "--color=never"], cwd=project,
                capture_output=True, text=True, timeout=scaled(120))
            assert tested.returncode == 1, (profile, tested)
            text = tested.stdout + tested.stderr
            assert "Failed counts-failures" in text, text
            assert "Passed counts-failures" not in text, text
        binary = root / "failure"
        built = subprocess.run(
            [str(compiler), str(fixtures / "hosted_exit_status_owned.gti"),
             "-O3", "-o", str(binary)], capture_output=True, text=True,
            timeout=scaled(120))
        assert built.returncode == 0, built.stderr
        failed = subprocess.run([str(binary), *("x",) * 8],
                                capture_output=True, text=True, timeout=scaled(30))
        assert failed.returncode == 70, failed
        assert failed.stdout == "cleanup\n", failed.stdout
        assert "GTI-R0002" in failed.stderr, failed.stderr


if __name__ == "__main__":
    main()
