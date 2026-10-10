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
memory_jobs="${detected_jobs}"
per_job="${GTI_MEMORY_PER_JOB_MIB:-}"
if [[ -n "${per_job}" ]]; then
  if [[ ! "${per_job}" =~ ^[1-9][0-9]*$ ]]; then
    echo "GTI_MEMORY_PER_JOB_MIB must be a positive integer" >&2
    exit 1
  fi
  if [[ -n "${memory_mib}" ]]; then
    memory_jobs=$(( memory_mib / per_job ))
    if (( memory_jobs < 1 )); then
      memory_jobs=1
    fi
    if (( memory_jobs < detected_jobs )); then
      echo "Memory bounds compile jobs: ${memory_mib} MiB visible, ${per_job} MiB per job" >&2
    fi
  fi
fi
if (( memory_jobs < detected_jobs )); then
  default_jobs="${memory_jobs}"
else
  default_jobs="${detected_jobs}"
fi

jobs="${GTI_CONFIGURED_JOBS:-${default_jobs}}"
test_jobs="${GTI_CONFIGURED_TEST_JOBS:-${detected_jobs}}"
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
