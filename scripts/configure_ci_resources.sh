#!/usr/bin/env bash
set -euo pipefail

# A container's visible CPU count need not match its CPU/memory quota, and
# CI/nightly/stable jobs can share the same host. Raise these explicit budgets
# only after provisioning the runner; never multiply host CPUs per job.
jobs="${GTI_CONFIGURED_JOBS:-2}"
test_jobs="${GTI_CONFIGURED_TEST_JOBS:-1}"
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
  if command -v sccache >/dev/null 2>&1; then
    echo "CMAKE_C_COMPILER_LAUNCHER=sccache"
    echo "CMAKE_CXX_COMPILER_LAUNCHER=sccache"
    if [[ -n "${ACTIONS_CACHE_URL:-}" && -n "${ACTIONS_RUNTIME_TOKEN:-}" ]]; then
      echo "SCCACHE_GHA_ENABLED=true"
    fi
  fi
} >> "${GITEA_ENV}"
echo "Runner budget: ${jobs} compile jobs, ${test_jobs} CTest workers"
