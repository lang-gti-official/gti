#!/usr/bin/env python3
"""Explicit resource limits are deterministic and fail closed."""

import os
import pathlib
import subprocess
import sys
import tempfile


def main():
    script = pathlib.Path(sys.argv[1]).resolve()
    with tempfile.TemporaryDirectory(prefix="gti-ci-resources-") as temporary:
        for index, (jobs, test_jobs, expected) in enumerate((
            ("", "", ("2", "1")),
            ("3", "2", ("3", "2")),
            ("0", "1", None),
            ("2", "-1", None),
            ("auto", "1", None),
            ("2", "1\nBAD=value", None),
        )):
            output = pathlib.Path(temporary) / str(index)
            environment = os.environ.copy()
            environment.update(GTI_CONFIGURED_JOBS=jobs,
                               GTI_CONFIGURED_TEST_JOBS=test_jobs,
                               GITEA_ENV=str(output))
            result = subprocess.run(["bash", str(script)], env=environment,
                                    capture_output=True, text=True, timeout=10)
            if expected is None:
                assert result.returncode != 0, result
                assert not output.exists(), "invalid input wrote runner state"
            else:
                assert result.returncode == 0, result.stderr
                values = dict(line.split("=", 1)
                              for line in output.read_text().splitlines())
                assert values["GTI_JOBS"] == expected[0]
                assert values["GTI_TEST_JOBS"] == expected[1]
    print("CI resource budgets passed")


if __name__ == "__main__":
    main()
