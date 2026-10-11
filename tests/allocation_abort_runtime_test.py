#!/usr/bin/env python3
"""Exhaustion aborts without cleanup, observer arbitration, or worker handoff."""

import os
import signal
import subprocess
import sys
from harness_timeouts import scaled


# The Windows CRT's abort exit code is 3; POSIX subprocess reports SIGABRT.
ABORT_STATUS = 3 if os.name == "nt" else -signal.SIGABRT


def main():
    helper = sys.argv[1]
    report = (b"allocation.gti:17: runtime error[GTI-R0011]: "
              b"allocation_failure in private_storage\n")
    for mode in ("direct", "worker", "observer", "invalid", "closed", "broken_pipe"):
        expected = (b"" if mode in ("closed", "broken_pipe") else
                    report.replace(b"allocation.gti:17", b"<runtime>:0")
                    if mode == "invalid" else report)
        result = subprocess.run([helper, mode], capture_output=True, timeout=scaled(10),
                                env=dict(os.environ, GTI_COLOR="never"))
        assert (result.returncode, result.stdout, result.stderr) == (ABORT_STATUS, b"B", expected), (
            mode, result)
    print("Allocation abort: direct, managed worker, observer reentry, invalid record, "
          "closed stderr and broken pipe passed; no cleanup or allocation")


if __name__ == "__main__":
    main()
