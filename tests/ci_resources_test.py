#!/usr/bin/env python3
"""Available-CPU defaults and independent resource overrides fail closed."""

import os
import pathlib
import shutil
import subprocess
import sys
import tempfile


def main():
    script = pathlib.Path(sys.argv[1]).resolve()
    bash = shutil.which("bash")
    assert bash, "bash is required"
    with tempfile.TemporaryDirectory(prefix="gti-ci-resources-") as temporary:
        root = pathlib.Path(temporary)
        commands = root / "bin"
        commands.mkdir()
        detector = commands / "nproc"
        detector.write_text('#!/bin/sh\n[ "$#" = 0 ] || exit 1\nprintf "12\\n"\n')
        detector.chmod(0o755)
        for index, (jobs, test_jobs, expected) in enumerate((
            ("", "", ("12", "12")),
            ("3", "2", ("3", "2")),
            ("3", "", ("3", "12")),
            ("", "2", ("12", "2")),
            ("0", "1", None),
            ("2", "-1", None),
            ("auto", "1", None),
            ("2", "1\nBAD=value", None),
        )):
            output = root / str(index)
            environment = os.environ.copy()
            environment.update(GTI_CONFIGURED_JOBS=jobs,
                               GTI_CONFIGURED_TEST_JOBS=test_jobs,
                               GITEA_ENV=str(output), PATH=str(commands))
            result = subprocess.run([bash, str(script)], env=environment,
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
                assert set(values) == {"GTI_JOBS", "GTI_TEST_JOBS"}

        # Detection failures are conservative, not invalid runner-state writes.
        for index, body in enumerate(('exit 1', 'printf "0\\n"',
                                      'printf "bad\\n"', 'printf "1\\nBAD=value\\n"')):
            detector.write_text(f"#!/bin/sh\n{body}\n")
            output = root / f"fallback-{index}"
            environment.update(GTI_CONFIGURED_JOBS="", GTI_CONFIGURED_TEST_JOBS="",
                               GITEA_ENV=str(output))
            subprocess.run([bash, str(script)], env=environment, check=True,
                           capture_output=True, text=True, timeout=10)
            assert output.read_text() == "GTI_JOBS=1\nGTI_TEST_JOBS=1\n"

        detector.unlink()
        for command, arguments in (("sysctl", "-n hw.logicalcpu"),
                                   ("getconf", "_NPROCESSORS_ONLN")):
            detector = commands / command
            detector.write_text(f'#!/bin/sh\n[ "$*" = "{arguments}" ] || exit 1\n'
                                'printf "8\\n"\n')
            detector.chmod(0o755)
            output = root / command
            environment["GITEA_ENV"] = str(output)
            subprocess.run([bash, str(script)], env=environment, check=True,
                           capture_output=True, text=True, timeout=10)
            assert output.read_text() == "GTI_JOBS=8\nGTI_TEST_JOBS=8\n"
            detector.unlink()
        output = root / "no-detector"
        environment["GITEA_ENV"] = str(output)
        subprocess.run([bash, str(script)], env=environment, check=True,
                       capture_output=True, text=True, timeout=10)
        assert output.read_text() == "GTI_JOBS=1\nGTI_TEST_JOBS=1\n"
    print("CI available-CPU defaults, overrides, and detection fallbacks passed")


if __name__ == "__main__":
    main()
