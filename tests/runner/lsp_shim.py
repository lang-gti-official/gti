#!/usr/bin/env python3
"""A stand-in language server for gti-test's self-test (tests/runner/check.py).

It runs the real server (GTI_TEST_SHIM_SERVER, with this script's arguments)
and relays standard input and output, changed by GTI_TEST_SHIM_MODE:
  slow  every chunk the server writes waits GTI_TEST_SHIM_SECONDS first:
        wall time without CPU, which the CPU budget must not count;
  spin  every chunk carrying didOpen costs GTI_TEST_SHIM_SECONDS of this
        process's CPU, which the budget must count;
  hang  publishDiagnostics notifications are dropped, so the test waits
        until the hang guard.
gti-test measures this process's CPU (the real server is its child).
"""

import os
import subprocess
import sys
import threading
import time

MODE = os.environ.get("GTI_TEST_SHIM_MODE", "")
SECONDS = float(os.environ.get("GTI_TEST_SHIM_SECONDS", "1"))


def spin(seconds):
    end = time.process_time() + seconds
    while time.process_time() < end:
        pass


def main():
    server = subprocess.Popen([os.environ["GTI_TEST_SHIM_SERVER"], *sys.argv[1:]],
                              stdin=subprocess.PIPE, stdout=subprocess.PIPE)

    def forward_input():
        while True:
            chunk = os.read(0, 65536)
            if not chunk:
                break
            if MODE == "spin" and b"textDocument/didOpen" in chunk:
                spin(SECONDS)
            server.stdin.write(chunk)
            server.stdin.flush()
        server.stdin.close()

    threading.Thread(target=forward_input, daemon=True).start()
    while True:
        chunk = os.read(server.stdout.fileno(), 65536)
        if not chunk:
            break
        if MODE == "slow":
            time.sleep(SECONDS)
        if MODE == "hang" and b"publishDiagnostics" in chunk:
            continue
        os.write(1, chunk)
    sys.exit(server.wait())


if __name__ == "__main__":
    main()
