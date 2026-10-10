#!/usr/bin/env bash
set -euo pipefail

# Use the processors available to the job, not nproc --all (the whole host).
# Runner provisioning owns CPU/memory quotas. Explicit repository overrides
# remain useful for a shared host or a memory-heavy configuration.
detected_jobs=1
if command -v nproc >/dev/null 2>&1; then
  detected_jobs="$(nproc 2>/dev/null || printf '1')"
elif command -v sysctl >/dev/null 2>&1; then
  detected_jobs="$(sysctl -n hw.logicalcpu 2>/dev/null || printf '1')"
elif command -v getconf >/dev/null 2>&1; then
  detected_jobs="$(getconf _NPROCESSORS_ONLN 2>/dev/null || printf '1')"
fi
if [[ ! "${detected_jobs}" =~ ^[1-9][0-9]*$ ]]; then
  detected_jobs=1
fi

# A job's memory bounds compile parallelism as much as its processors do:
# one optimizing compile of the largest GTI translation unit, or of a bundled
# LLVM unit beside it, needs a few GiB, and the Linux OOM killer ends the
# whole build when every processor runs one at once. When the workflow states
# the memory one compile job needs (GTI_MEMORY_PER_JOB_MIB), the detected
# default is capped by the memory the job can see. Explicit overrides are not
# capped: they are the operator's statement about the runner. Detection reads
# /proc/meminfo with shell builtins only; a host without it keeps the
# processor count.
memory_mib=""
meminfo="${GTI_MEMINFO:-/proc/meminfo}"
if [[ -r "${meminfo}" ]]; then
  while read -r key value _; do
    if [[ "${key}" == "MemTotal:" && "${value}" =~ ^[0-9]+$ ]]; then
      memory_mib=$(( value / 1024 ))
      break
    fi
  done < "${meminfo}"
fi
# The same bound applies to CTest workers when the workflow states the memory
# one test process needs (GTI_MEMORY_PER_TEST_MIB): sanitizer-instrumented
# suites grow several times larger than their plain builds.
memory_bound() {
  local name="$1" budget="$2" what="$3" bound="${detected_jobs}"
  if [[ -n "${budget}" ]]; then
    if [[ ! "${budget}" =~ ^[1-9][0-9]*$ ]]; then
      echo "${name} must be a positive integer" >&2
      exit 1
    fi
    if [[ -n "${memory_mib}" ]]; then
      bound=$(( memory_mib / budget ))
      if (( bound < 1 )); then
        bound=1
      fi
      if (( bound < detected_jobs )); then
        echo "Memory bounds ${what}: ${memory_mib} MiB visible, ${budget} MiB each" >&2
      else
        bound="${detected_jobs}"
      fi
    fi
  fi
  printf '%s\n' "${bound}"
}
default_jobs="$(memory_bound GTI_MEMORY_PER_JOB_MIB "${GTI_MEMORY_PER_JOB_MIB:-}" "compile jobs")"
default_test_jobs="$(memory_bound GTI_MEMORY_PER_TEST_MIB "${GTI_MEMORY_PER_TEST_MIB:-}" "CTest workers")"

jobs="${GTI_CONFIGURED_JOBS:-${default_jobs}}"
test_jobs="${GTI_CONFIGURED_TEST_JOBS:-${default_test_jobs}}"
for value in "${jobs}" "${test_jobs}"; do
  if [[ ! "${value}" =~ ^[1-9][0-9]*$ ]]; then
    echo "GTI_JOBS and GTI_TEST_JOBS must be positive integers" >&2
    exit 1
  fi
done
: "${GITEA_ENV:?GITEA_ENV must name the runner environment file}"
{
  echo "GTI_JOBS=${jobs}"
  echo "GTI_TEST_JOBS=${test_jobs}"
} >> "${GITEA_ENV}"
echo "Runner budget: ${jobs} compile jobs, ${test_jobs} CTest workers (${detected_jobs} processors, ${memory_mib:-unknown} MiB)"
