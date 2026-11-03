#!/usr/bin/env bash
# Run only on an existing, disposable Ubuntu image-builder machine.
set -euo pipefail
if [[ ${1:-} != --approve-image-changes || $# != 3 ]]; then
  echo 'usage: prepare_image.sh --approve-image-changes LLVM_HTTPS_ARCHIVE SHA256' >&2
  exit 2
fi
if [[ $(id -u) != 0 || $(uname -m) != x86_64 ]]; then
  echo 'requires root on an existing disposable x86_64 Ubuntu 24.04 image builder' >&2
  exit 2
fi
source /etc/os-release
if [[ $ID != ubuntu || $VERSION_ID != 24.04 || $2 != https://* || ! $3 =~ ^[0-9a-f]{64}$ ]]; then
  echo 'requires Ubuntu 24.04 and a reviewed HTTPS LLVM archive with exact SHA256' >&2
  exit 2
fi
if [[ -e /opt/gti-image-receipt.json || -e /opt/llvm ]]; then
  echo 'image already prepared; start from a clean pinned base image, do not update it in place' >&2
  exit 2
fi
export DEBIAN_FRONTEND=noninteractive
apt-get update
apt-get install -y --no-install-recommends python3 git cmake ninja-build gcc-14 g++-14 \
  ca-certificates curl openssh-server google-compute-engine-oslogin google-guest-agent \
  libssl-dev libffi-dev libxml2-dev libzstd-dev zlib1g-dev libncurses-dev xz-utils
temporary=$(mktemp -d)
trap 'rm -rf -- "$temporary"' EXIT
curl --fail --location --proto '=https' --tlsv1.2 --max-time 600 "$2" -o "$temporary/llvm.tar.xz"
printf '%s  %s\n' "$3" "$temporary/llvm.tar.xz" | sha256sum --check -
mkdir -p /opt/llvm
tar --extract --xz --file "$temporary/llvm.tar.xz" --directory /opt/llvm --strip-components=1 --no-same-owner --no-same-permissions
ln -s /opt/llvm/bin/llvm-config /usr/local/bin/llvm-config
dpkg-query -W > /opt/gti-image-packages.txt
python3 - "$2" "$3" <<'PY'
import json, pathlib, subprocess, sys
version = subprocess.check_output(['/opt/llvm/bin/llvm-config', '--version'], text=True).strip()
pathlib.Path('/opt/gti-image-receipt.json').write_text(json.dumps({
    'schema': 1, 'llvm_url': sys.argv[1], 'llvm_sha256': sys.argv[2], 'llvm_version': version,
    'packages': '/opt/gti-image-packages.txt', 'note': 'Owner captures an immutable, versioned image; no bit-for-bit rebuild claim.'
}, indent=2))
PY
echo 'Prepared tools only. Remove temporary login credentials, capture a versioned image, and delete the builder/unused disks explicitly.'
