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
jobs="${GTI_CONFIGURED_JOBS:-${detected_jobs}}"
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
echo "Runner budget: ${jobs} compile jobs, ${test_jobs} CTest workers"
