#!/usr/bin/env bash
set -euo pipefail

# Install and export one reviewed native toolchain for a hosted CI job. The
# runtime's raw atomic lanes admit only reviewed compiler profiles
# (runtime/src/atomic_lane.h), and the driver compiles test programs with the
# exported CC/CXX, so CMake and every test use the same compiler. LLVM is
# the pinned major (CMakeLists.txt GTI_PINNED_LLVM_VERSION): CMake refuses any
# other, so CI tests the LLVM that releases bundle and developers build with.
llvm_major="$(sed -n 's/^set(GTI_PINNED_LLVM_VERSION "\([0-9]*\)\..*/\1/p' "$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)/CMakeLists.txt")"
: "${llvm_major:?CMakeLists.txt must set GTI_PINNED_LLVM_VERSION}"
# The apt.llvm.org signing key; the download is trusted only with this
# fingerprint.
llvm_apt_key_fingerprint=6084F3CF814B57C1CF12EFD515CF4D18AF4F7421
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
    # GCC 14 is the reviewed Linux x86_64 profile. Ubuntu does not package
    # LLVM ${llvm_major}, so it comes from LLVM's own apt repository for this
    # release (its LLVM ${llvm_major} release branch). The Clang CMake package
    # (and the clang the LLVM tests run) is in clang-${llvm_major}, not
    # libclang-${llvm_major}-dev, and clang's sanitizer runtimes (compiler-rt,
    # which the ThreadSanitizer harnesses need) are in
    # libclang-rt-${llvm_major}-dev, which --no-install-recommends leaves out.
    # shellcheck disable=SC1091
    codename="$(. /etc/os-release && echo "${VERSION_CODENAME}")"
    key="$(mktemp)"
    curl --proto '=https' --tlsv1.2 -fsSL --retry 5 --retry-all-errors \
      https://apt.llvm.org/llvm-snapshot.gpg.key -o "${key}"
    fingerprint="$(gpg --show-keys --with-colons "${key}" | awk -F: '/^fpr/ {print $10; exit}')"
    if [[ "${fingerprint}" != "${llvm_apt_key_fingerprint}" ]]; then
      echo "apt.llvm.org key fingerprint ${fingerprint:-missing} is not ${llvm_apt_key_fingerprint}" >&2
      exit 1
    fi
    sudo install -d -m 0755 /etc/apt/keyrings
    sudo gpg --batch --yes --dearmor -o /etc/apt/keyrings/apt.llvm.org.gpg "${key}"
    rm -f "${key}"
    echo "deb [signed-by=/etc/apt/keyrings/apt.llvm.org.gpg] https://apt.llvm.org/${codename}/ llvm-toolchain-${codename}-${llvm_major} main" |
      sudo tee /etc/apt/sources.list.d/apt.llvm.org.list >/dev/null
    sudo apt-get update
    sudo apt-get install -y --no-install-recommends \
      build-essential ccache "clang-${llvm_major}" cmake g++-14 gcc-14 ninja-build \
      libcurl4-openssl-dev "llvm-${llvm_major}-dev" "libclang-${llvm_major}-dev" \
      "libclang-rt-${llvm_major}-dev"
    cc=gcc-14
    cxx=g++-14
    llvm_dir="/usr/lib/llvm-${llvm_major}/lib/cmake/llvm"
    ;;
  Darwin)
    # Command Line Tools 27 carry Apple clang 21, the reviewed macOS profile;
    # an older selected Xcode does not.
    brew install ccache ninja "llvm@${llvm_major}"
    if [[ "$(/usr/bin/c++ -dM -E -x c++ /dev/null | sed -n 's/^#define __clang_major__ //p')" != 21 &&
          -d /Library/Developer/CommandLineTools ]]; then
      sudo xcode-select --switch /Library/Developer/CommandLineTools
    fi
    cc=/usr/bin/cc
    cxx=/usr/bin/c++
    llvm_dir="$(brew --prefix "llvm@${llvm_major}")/lib/cmake/llvm"
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
