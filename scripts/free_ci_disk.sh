#!/usr/bin/env bash
# Free disk on a hosted Linux runner before a sanitized build.
#
# An ASan/UBSan tree of the compiler and its tests outgrew the free space of
# the ubuntu-24.04 hosted image (full validation 65: "final link failed: No
# space left on device"). The image ships about 25 GB of toolchains this
# repository never uses; removing them is the usual remedy. Nothing GTI needs
# lives under these paths. Elsewhere (macOS, self-hosted) this does nothing.
set -euo pipefail
if [[ "$(uname -s)" != Linux || "${RUNNER_ENVIRONMENT:-}" != github-hosted ]]; then
  echo "free_ci_disk: not a GitHub-hosted Linux runner; nothing to do"
  exit 0
fi
df -h /
for path in /usr/share/dotnet /usr/local/lib/android /opt/ghc /usr/local/.ghcup \
            /opt/hostedtoolcache/CodeQL /usr/local/share/boost /usr/share/swift; do
  [[ -e "${path}" ]] && sudo rm -rf "${path}"
done
df -h /
