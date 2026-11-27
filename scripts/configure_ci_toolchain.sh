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
    # tree needs more. Drop preinstalled toolchains GTI never uses. A job that
    # only configures (GTI_CI_CONFIGURE_ONLY=1, the gate's tree-consistency
    # run) builds nothing and skips the minute and a half this takes.
    if [[ "${RUNNER_ENVIRONMENT:-}" == github-hosted && "${GTI_CI_CONFIGURE_ONLY:-}" != 1 ]]; then
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
    # Keep Ubuntu's configured mirrors and signing policy. Hosted runners also
    # have repositories for unrelated products, which can fail independently.
    apt_sources="$(mktemp -d)"
    trap 'rm -rf -- "${apt_sources}"' EXIT
    cp /etc/apt/sources.list.d/ubuntu.sources "${apt_sources}/ubuntu.sources"
    add_llvm_source() {
      echo "deb [signed-by=/etc/apt/keyrings/apt.llvm.org.gpg] https://apt.llvm.org/${codename}/ llvm-toolchain-${codename}-$1 main" >> "${apt_sources}/llvm.list"
    }
    add_llvm_source "${llvm_major}"
    # A stalled mirror once held an integration shard in `apt-get update`
    # until the six-hour job limit. Bound each attempt and retry instead.
    apt_get() {
      local attempt limit=300
      # Installing also unpacks/configures LLVM; allow that local work more
      # time than an index refresh, while retaining bounded network requests.
      if [[ "$1" == install ]]; then limit=900; fi
      for attempt in 1 2 3 4; do
        if sudo timeout --kill-after=30 "${limit}" apt-get -o Acquire::Retries=3 \
             -o Acquire::http::Timeout=30 -o Acquire::https::Timeout=30 \
             -o Dir::Etc::sourcelist=/dev/null -o "Dir::Etc::sourceparts=${apt_sources}" \
             -o APT::Get::List-Cleanup=false "$@"; then
          return 0
        fi
        echo "apt-get $1: attempt ${attempt} failed or exceeded ${limit} s" >&2
        if (( attempt == 4 )); then break; fi
        if [[ "$1" == install ]]; then
          # timeout can interrupt dpkg mid-transaction. Repeating apt-get
          # without recovery then fails immediately on every remaining try.
          # Missing dependencies can make configuration fail; the next apt
          # attempt must still succeed before we accept the installation.
          if ! sudo timeout --kill-after=30 300 dpkg --configure -a; then
            echo "dpkg recovery incomplete; retrying apt-get install" >&2
          fi
        fi
        sleep $((attempt * 15))
      done
      echo "apt-get $1 did not succeed after 4 attempts" >&2
      return 1
    }
    apt_get update
    apt_get install -y --no-install-recommends \
      build-essential ccache "clang-${llvm_major}" cmake g++-14 gcc-14 ninja-build \
      libcurl4-openssl-dev "llvm-${llvm_major}-dev" "libclang-${llvm_major}-dev" \
      "libclang-rt-${llvm_major}-dev"
    cc=gcc-14
    cxx=g++-14
    if [[ "${GTI_CI_COVERAGE:-}" == 1 ]]; then
      # Clang source-based coverage (suite-measures.yml) needs a Clang host
      # compiler, and the runtime's raw atomic lanes admit on Linux x86_64 only
      # the Clang major runtime/src/atomic_lane.h names, which can differ from
      # the pinned LLVM. That Clang, its profile runtime and its
      # llvm-profdata/llvm-cov come from the same repository and key, on that
      # major's release line. The compiler still links the pinned LLVM.
      coverage_major="$(sed -n '/defined(__linux__) && defined(__x86_64__)/,/#else/s/.*__clang_major__ != \([0-9][0-9]*\).*/\1/p' \
        "${repository}/runtime/src/atomic_lane.h" | head -n 1)"
      : "${coverage_major:?runtime/src/atomic_lane.h names no reviewed Linux Clang}"
      if [[ "${coverage_major}" != "${llvm_major}" ]]; then
        add_llvm_source "${coverage_major}"
        apt_get update
      fi
      apt_get install -y --no-install-recommends \
        "clang-${coverage_major}" "llvm-${coverage_major}" "libclang-rt-${coverage_major}-dev"
      coverage_bin="/usr/lib/llvm-${coverage_major}/bin"
      cc="${coverage_bin}/clang"
      cxx="${coverage_bin}/clang++"
      echo "GTI_COVERAGE_LLVM_BIN=${coverage_bin}" >> "${GTI_CI_ENV_FILE}"
    fi
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
