"""Persisted keys survive fresh frontend processes and relocated inputs."""
import subprocess
import sys

first = subprocess.check_output([sys.argv[1], "--original"])
second = subprocess.check_output([sys.argv[1], "--relocated"])
assert first == second, (first, second)
assert first.startswith(b"gti.function-key.1:"), first
print("persistent identity agrees across independent processes")
