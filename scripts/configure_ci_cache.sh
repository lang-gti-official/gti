#!/usr/bin/env bash
set -euo pipefail

# Workflows install ccache explicitly and persist only this object store, never
# a build tree, installed toolchain, test result, or unverified LLVM source.
: "${GITEA_ENV:?GITEA_ENV must name the runner environment file}"
: "${GTI_CACHE_DIRECTORY:?GTI_CACHE_DIRECTORY must name the object cache}"
if [[ "${GTI_CACHE_DIRECTORY}" != /* || "${GTI_CACHE_DIRECTORY}" == / ||
      "${GTI_CACHE_DIRECTORY}" == *$'\n'* ||
      "${GTI_CACHE_DIRECTORY}" == *$'\r'* ]]; then
  echo "GTI_CACHE_DIRECTORY must be a dedicated absolute, single-line path" >&2
  exit 1
fi
cache_launcher="$(command -v ccache || true)"
if [[ -z "${cache_launcher}" || "${cache_launcher}" != /* ||
      "${cache_launcher}" == *$'\n'* || "${cache_launcher}" == *$'\r'* ]]; then
  echo "Install ccache before configuring the CI object cache" >&2
  exit 1
fi
{
  echo "CMAKE_C_COMPILER_LAUNCHER=${cache_launcher}"
  echo "CMAKE_CXX_COMPILER_LAUNCHER=${cache_launcher}"
  echo "CCACHE_DIR=${GTI_CACHE_DIRECTORY}"
  echo "CCACHE_MAXSIZE=10G"
  echo "CCACHE_COMPILERCHECK=content"
  # No path rewriting, omitted inputs, or PCH/time-macro correctness relaxations.
  echo "CCACHE_BASEDIR="
  echo "CCACHE_SLOPPINESS="
} >> "${GITEA_ENV}"
echo "Compiler object cache: ${GTI_CACHE_DIRECTORY} (10G, compiler-content identity)"
