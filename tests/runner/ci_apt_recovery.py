#!/usr/bin/env python3
"""Exercise CI apt source selection, bounded retries and interrupted dpkg recovery."""

from pathlib import Path
import subprocess
import sys
import tempfile

source = Path(sys.argv[1]).read_text()
wrapper = source.split("    apt_get() {", 1)[1].split("    apt_get update", 1)[0]
wrapper = "apt_get() {" + wrapper
# Shell functions replace all privileged operations and waits. Never invoke the
# machine's apt, dpkg or sudo; exercise the production retry function verbatim.
fakes = r'''
set -euo pipefail
mode="$1"
operation="$2"
apt_sources="/tmp/CI sources"
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
      if [[ "$mode" == unrelated_feed ]]; then
        # The runner's default sources contain an unrelated feed returning 403.
        # APT reads both the main list and sourceparts on update and install.
        shift
        local list="" parts=""
        while [[ "${1:-}" == -o ]]; do
          case "$2" in
            Dir::Etc::sourcelist=*) list="${2#*=}" ;;
            Dir::Etc::sourceparts=*) parts="${2#*=}" ;;
          esac
          shift 2
        done
        if [[ "$list" != /dev/null || "$parts" != "$apt_sources" ]]; then
          echo '403 Forbidden from unrelated runner repository' >&2
          return 100
        fi
        return 0
      fi
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

check("unrelated_feed", "update", True, 1, 0)
check("unrelated_feed", "install", True, 1, 0)
check("interrupted", "install", True, 2, 1)
check("success", "install", True, 1, 0)
check("success", "update", True, 1, 0)
check("missing_dependency", "install", True, 2, 1)
check("broken_recovery", "install", False, 4, 3)
check("persistent", "install", False, 4, 3)
check("persistent", "update", False, 4, 0)
# Capture the actual source files APT receives in the normal and coverage jobs.
# Use a miniature runner configuration so this also runs on macOS.
setup = source.split('    rm -f "${key}"', 1)[1].split('    # A stalled mirror', 1)[0]
coverage = source.split('      : "${coverage_major:?', 1)[1].split("\n", 1)[1].split("      apt_get install", 1)[0]
with tempfile.TemporaryDirectory() as temporary:
    root = Path(temporary)
    sources = root / "sources.list.d"
    sources.mkdir()
    ubuntu = "Types: deb\nURIs: https://runner-mirror.example/ubuntu\nSuites: noble noble-security\n"
    (sources / "ubuntu.sources").write_text(ubuntu)
    (sources / "microsoft.list").write_text("deb https://unrelated.example noble main\n")
    script = ('set -eu\ncodename=noble\nllvm_major=23\ncoverage_major=20\n'
              'apt_get() { test "$1" = update; }\n' + setup +
              'cp -R "${apt_sources}" normal\n' + coverage + 'cp -R "${apt_sources}" coverage\n')
    subprocess.run(["bash", "-c", script.replace("/etc/apt/sources.list.d", str(sources))],
                   cwd=root, check=True, timeout=5)
    llvm = "deb [signed-by=/etc/apt/keyrings/apt.llvm.org.gpg] https://apt.llvm.org/noble/ llvm-toolchain-noble-"
    for job, majors in (("normal", [23]), ("coverage", [23, 20])):
        entries = {p.name: p.read_text() for p in (root / job).iterdir()}
        assert entries == {"ubuntu.sources": ubuntu,
                           "llvm.list": "".join(f"{llvm}{major} main\n" for major in majors)}, entries

print("CI apt source selection, coverage and retry cases passed")
