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

        # Memory caps only the detected compile default, only when the
        # workflow states a per-job budget, and never the CTest workers or an
        # explicit operator override. Detection uses the stated meminfo file.
        detector = commands / "nproc"
        detector.write_text('#!/bin/sh\nprintf "12\\n"\n')
        detector.chmod(0o755)
        meminfo = root / "meminfo"
        meminfo.write_text("MemTotal:        8000000 kB\nMemFree:          100 kB\n")
        for index, (per_job, jobs, expected) in enumerate((
            ("", "", ("12", "12")),          # no budget: processors only
            ("2048", "", ("3", "12")),       # 7812 MiB / 2048 -> 3 compile jobs
            ("512", "", ("12", "12")),       # ample memory keeps the processors
            ("65536", "", ("1", "12")),      # never below one job
            ("2048", "9", ("9", "12")),      # explicit override is not capped
        )):
            output = root / f"memory-{index}"
            environment.update(GTI_CONFIGURED_JOBS=jobs, GTI_CONFIGURED_TEST_JOBS="",
                               GTI_MEMORY_PER_JOB_MIB=per_job, GTI_MEMINFO=str(meminfo),
                               GITEA_ENV=str(output))
            result = subprocess.run([bash, str(script)], env=environment, check=True,
                                    capture_output=True, text=True, timeout=10)
            assert output.read_text() == f"GTI_JOBS={expected[0]}\nGTI_TEST_JOBS={expected[1]}\n", result
            assert "7812 MiB" in result.stdout, result.stdout
        environment.update(GTI_CONFIGURED_JOBS="")
        for per_job in ("0", "-1", "lots"):
            output = root / f"memory-invalid-{per_job}"
            environment.update(GTI_MEMORY_PER_JOB_MIB=per_job, GITEA_ENV=str(output))
            result = subprocess.run([bash, str(script)], env=environment,
                                    capture_output=True, text=True, timeout=10)
            assert result.returncode != 0 and not output.exists(), per_job
        # CTest workers have their own budget, independent of compile jobs.
        for index, (per_job, per_test, test_jobs, expected) in enumerate((
            ("", "4096", "", ("12", "1")),      # 7812 / 4096 -> 1 worker
            ("2048", "2048", "", ("3", "3")),
            ("", "1024", "", ("12", "7")),
            ("", "4096", "5", ("12", "5")),     # explicit workers are not capped
        )):
            output = root / f"memory-test-{index}"
            environment.update(GTI_MEMORY_PER_JOB_MIB=per_job, GTI_MEMORY_PER_TEST_MIB=per_test,
                               GTI_CONFIGURED_TEST_JOBS=test_jobs, GITEA_ENV=str(output))
            result = subprocess.run([bash, str(script)], env=environment, check=True,
                                    capture_output=True, text=True, timeout=10)
            assert output.read_text() == f"GTI_JOBS={expected[0]}\nGTI_TEST_JOBS={expected[1]}\n", result
        environment.update(GTI_MEMORY_PER_TEST_MIB="many", GITEA_ENV=str(root / "memory-test-invalid"))
        result = subprocess.run([bash, str(script)], env=environment,
                                capture_output=True, text=True, timeout=10)
        assert result.returncode != 0 and "GTI_MEMORY_PER_TEST_MIB" in result.stderr
        environment.update(GTI_MEMORY_PER_TEST_MIB="", GTI_CONFIGURED_TEST_JOBS="")
        output = root / "memory-unreadable"
        environment.update(GTI_MEMORY_PER_JOB_MIB="2048", GTI_MEMINFO=str(root / "absent"),
                           GITEA_ENV=str(output))
        result = subprocess.run([bash, str(script)], env=environment, check=True,
                                capture_output=True, text=True, timeout=10)
        assert output.read_text() == "GTI_JOBS=12\nGTI_TEST_JOBS=12\n"
        assert "unknown MiB" in result.stdout, result.stdout
    print("CI available-CPU defaults, memory caps, overrides, and detection fallbacks passed")


if __name__ == "__main__":
    main()
