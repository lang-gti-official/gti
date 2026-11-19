#!/usr/bin/env python3
"""Prove bounded CI apt retries recover interrupted dpkg without hiding failures."""

from pathlib import Path
import subprocess
import sys

source = Path(sys.argv[1]).read_text()
wrapper = source.split("    apt_get() {", 1)[1].split("    apt_get update", 1)[0]
wrapper = "apt_get() {" + wrapper
# Shell functions replace all privileged operations and waits. Never invoke the
# machine's apt, dpkg or sudo; exercise the production retry function verbatim.
fakes = r'''
set -euo pipefail
mode="$1"
operation="$2"
calls=0
interrupted=0
sleep() { echo "sleep $*"; }
sudo() {
  echo "sudo $*"
  [[ "$1" == timeout ]] || return 90
  shift
  if [[ "$1" == --kill-after=30 ]]; then shift; fi
  [[ "$1" =~ ^[0-9]+$ ]] || return 91
  (( $1 > 0 && $1 <= 900 )) || return 92
  shift
  case "$1" in
    apt-get)
      calls=$((calls + 1))
      [[ "$mode" == success ]] && return 0
      [[ "$mode" == persistent ]] && return 100
      if (( calls == 1 )); then interrupted=1; return 124; fi
      (( interrupted == 0 ))
      ;;
    dpkg)
      [[ "$*" == 'dpkg --configure -a' ]] || return 92
      if [[ "$mode" == broken_recovery ]]; then return 124; fi
      interrupted=0
      # Configuration may need dependencies which the next apt install fixes.
      [[ "$mode" != missing_dependency ]]
      ;;
    *) return 93 ;;
  esac
}
'''

def check(mode, operation, success, attempts, repairs):
    arguments = ' -y --no-install-recommends test-package' if operation == "install" else ""
    result = subprocess.run(
        ["bash", "-c", fakes + wrapper + '\napt_get "$operation"' + arguments + '\n',
         "apt-recovery-test", mode, operation],
        capture_output=True, text=True, timeout=5, check=False)
    output = result.stdout + result.stderr
    assert (result.returncode == 0) == success, (mode, operation, result.returncode, output)
    commands = result.stdout.splitlines()
    apt = [line for line in commands if " apt-get " in line]
    dpkg = [line for line in commands if " dpkg " in line]
    assert len(apt) == attempts and len(dpkg) == repairs, output
    assert len([line for line in commands if line.startswith("sleep ")]) == attempts - 1, output
    budget = 900 if operation == "install" else 300
    assert all(line.startswith(f"sudo timeout --kill-after=30 {budget} apt-get ") for line in apt), output
    assert all(line.endswith(operation + arguments) for line in apt), output
    assert all("Acquire::Retries=3" in line and "Acquire::http::Timeout=30" in line
               and "Acquire::https::Timeout=30" in line for line in apt), output
    assert all(line == "sudo timeout --kill-after=30 300 dpkg --configure -a" for line in dpkg), output
    if repairs:
        assert commands.index(apt[0]) < commands.index(dpkg[0]), output
    if not success:
        assert "did not succeed after 4 attempts" in result.stderr, output

check("interrupted", "install", True, 2, 1)
check("success", "install", True, 1, 0)
check("success", "update", True, 1, 0)
check("missing_dependency", "install", True, 2, 1)
check("broken_recovery", "install", False, 4, 3)
check("persistent", "install", False, 4, 3)
check("persistent", "update", False, 4, 0)
print("CI apt recovery: 7 scenarios passed")
