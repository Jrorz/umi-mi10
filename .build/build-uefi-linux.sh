#!/bin/bash
# Build the Mu-Silicium UEFI inside the Lima Linux VM (run from the VM, via `limactl shell`).
#
# The UEFI step cannot be built on macOS: Mu-Basetools, the prebuilt EDK2 build tools the Mu tree
# downloads, are published for Linux and Windows only (uefi/README.md assumes a Linux host).
set -e
ROOT=/Users/jrace/Downloads/hackios-xnu-kernel-main
MU=$ROOT/Mu-Silicium
DEV=${1:-umi}

echo "=== installing the EDK2 build requirements"
sudo apt-get update -qq
sudo DEBIAN_FRONTEND=noninteractive apt-get install -y -qq \
	build-essential git python3 python3-venv python3-pip uuid-dev nasm acpica-tools \
	clang lld llvm >/dev/null

echo "=== python environment"
if [ ! -d "$ROOT/.venv-linux" ]; then
	python3 -m venv "$ROOT/.venv-linux"
fi
"$ROOT/.venv-linux/bin/pip" install -q -r "$MU/pip-requirements.txt"

echo "=== building $DEV"
cd "$MU"
export PATH="$ROOT/.venv-linux/bin:$PATH"
python3 build_uefi.py -d "$DEV" -r RELEASE

echo "=== output"
ls -la "$MU/Build/${DEV}Pkg/RELEASE_CLANGPDB/FV/"
