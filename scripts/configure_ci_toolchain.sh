#!/usr/bin/env bash
set -euo pipefail

# Install and export one reviewed native toolchain for a hosted CI job. The
# runtime's raw atomic lanes admit only reviewed compiler profiles
# (runtime/src/atomic_lane.h), and the driver compiles test programs with the
# exported CC/CXX, so CMake and every test use the same compiler. The LLVM
# support libraries need the DbgRecord API (LLVM 19 or newer).
GTI_CI_ENV_FILE="${GITHUB_ENV:-}"
: "${GTI_CI_ENV_FILE:?GITHUB_ENV must name the runner environment file}"
repository="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

case "$(uname -s)" in
  Linux)
    # A hosted runner starts with ~14 GiB free; a sanitizer link of the full
    # tree needs more. Drop preinstalled toolchains GTI never uses.
    if [[ "${RUNNER_ENVIRONMENT:-}" == github-hosted ]]; then
      sudo rm -rf /usr/share/dotnet /usr/local/lib/android /opt/ghc \
        /usr/local/.ghcup /opt/hostedtoolcache/CodeQL /usr/share/swift
      df -h / | tail -n 1
    fi
    # GCC 14 is the reviewed Linux x86_64 profile; LLVM 20 matches the pinned
    # release LLVM and ships in noble-updates. Ubuntu packages the Clang CMake
    # package (and the clang the LLVM tests run) in clang-20, not libclang-20-dev.
    sudo apt-get update
    sudo apt-get install -y --no-install-recommends \
      build-essential ccache clang-20 cmake g++-14 gcc-14 ninja-build \
      libcurl4-openssl-dev llvm-20-dev libclang-20-dev
    cc=gcc-14
    cxx=g++-14
    llvm_dir=/usr/lib/llvm-20/lib/cmake/llvm
    ;;
  Darwin)
    # Command Line Tools 27 carry Apple clang 21, the reviewed macOS profile;
    # an older selected Xcode does not.
    brew install ccache ninja llvm@22
    if [[ "$(/usr/bin/c++ -dM -E -x c++ /dev/null | sed -n 's/^#define __clang_major__ //p')" != 21 &&
          -d /Library/Developer/CommandLineTools ]]; then
      sudo xcode-select --switch /Library/Developer/CommandLineTools
    fi
    cc=/usr/bin/cc
    cxx=/usr/bin/c++
    llvm_dir="$(brew --prefix llvm@22)/lib/cmake/llvm"
    ;;
  *)
    echo "No reviewed CI toolchain for $(uname -s)" >&2
    exit 1
    ;;
esac

# Fail here, naming the compiler, rather than deep inside the build.
if ! printf '#include "atomic_lane.h"\n' |
     "${cxx}" -std=c++20 -fsyntax-only -I "${repository}/runtime/src" -x c++ -; then
  echo "$("${cxx}" --version | head -n 1) is not a reviewed GTI runtime compiler" >&2
  exit 1
fi
{
  echo "CC=${cc}"
  echo "CXX=${cxx}"
  echo "LLVM_DIR=${llvm_dir}"
} >> "${GTI_CI_ENV_FILE}"
echo "CI toolchain: $("${cxx}" --version | head -n 1); LLVM ${llvm_dir}"

# Editor checks must be registered on hosted runners, including PR builds.
python3 "${repository}/scripts/configure_ci_editor.py"
