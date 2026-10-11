"""A live borrowed worker must not escape a failed native join."""
import subprocess
import sys

result = subprocess.run([sys.argv[1], "join-failure"], capture_output=True,
                        text=True, timeout=30)
assert (result.returncode, result.stdout, result.stderr) == (73, "", ""), result
