#!/usr/bin/env bash
# Provision Ubuntu 24.04 (WSL2) with the StreamForge toolchain: Clang 20 + build dependencies.
# Run as root (wsl -d <distro> -u root bash /mnt/e/Workspace/StreamForge/scripts/setup-wsl.sh)
# or as a user with passwordless sudo.
set -euo pipefail

if [[ "$(id -u)" -eq 0 ]]; then
  APT=(apt-get)
else
  APT=(sudo -n apt-get)
fi

"${APT[@]}" update
DEBIAN_FRONTEND=noninteractive "${APT[@]}" install -y \
  ca-certificates curl wget gnupg ninja-build pkg-config \
  libssl-dev zlib1g-dev uuid-dev libsqlite3-dev sqlite3 python3 git cmake tzdata

if ! command -v clang-20 >/dev/null 2>&1; then
  KEYRING=/usr/share/keyrings/llvm-snapshot.gpg
  if [[ "$(id -u)" -eq 0 ]]; then
    curl -fsSL https://apt.llvm.org/llvm-snapshot.gpg.key | gpg --dearmor -o "$KEYRING"
    echo "deb [signed-by=$KEYRING] http://apt.llvm.org/noble/ llvm-toolchain-noble-20 main" \
      > /etc/apt/sources.list.d/llvm-20.list
  else
    curl -fsSL https://apt.llvm.org/llvm-snapshot.gpg.key | sudo -n gpg --dearmor -o "$KEYRING"
    echo "deb [signed-by=$KEYRING] http://apt.llvm.org/noble/ llvm-toolchain-noble-20 main" \
      | sudo -n tee /etc/apt/sources.list.d/llvm-20.list > /dev/null
  fi
  "${APT[@]}" update
  DEBIAN_FRONTEND=noninteractive "${APT[@]}" install -y clang-20 llvm-20 clang-format-20 clang-tidy-20 lld-20
fi

echo "== toolchain versions =="
clang-20 --version | head -1
clang++-20 --version | head -1
cmake --version | head -1
ninja --version
sqlite3 --version | head -1
pkg-config --exists uuid && echo "libuuid: ok"
echo "setup-wsl.sh: done"
