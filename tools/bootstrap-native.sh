#!/usr/bin/env bash
# Download signed MSYS2 CLANG64 packages into an isolated development toolchain.
set -euo pipefail
cd -- "$(dirname -- "$0")/.."
base="$PWD/builds/_internal/toolchain"
mkdir -p "$base/packages" "$base/db/local" "$base/keyring"
if [[ ! -f "$base/keyring/pubring.gpg" ]]; then
    cp -a /etc/pacman.d/gnupg/. "$base/keyring/"
fi
cat > "$base/pacman.conf" <<EOF
[options]
Architecture = auto
SigLevel = Required DatabaseOptional
DBPath = $base/db
CacheDir = $base/packages
GPGDir = $base/keyring
LogFile = $base/pacman.log
ParallelDownloads = 5
[clang64]
Server = https://repo.msys2.org/mingw/clang64/
EOF
names=(clang lld libc++ cmake ninja pkgconf python sdl3 boost fmt glslang spirv-cross
       spirv-headers vulkan-headers vulkan-loader vulkan-memory-allocator xxhash zydis
       robin-map ffmpeg protobuf nlohmann-json openssl miniupnpc)
packages=("${names[@]/#/mingw-w64-clang-x86_64-}")
pacman --config "$base/pacman.conf" -Syw --noconfirm "${packages[@]}"
pacman --config "$base/pacman.conf" -Sp --print-format '%f' "${packages[@]}" > "$base/packages.list"
while IFS= read -r filename; do
    archive="$base/packages/$filename"
    # Signed package payloads must stay beneath this toolchain.
    if tar -tf "$archive" | grep -vE '^(clang64/|usr/|\.(PKGINFO|BUILDINFO|MTREE|INSTALL)$)' | grep -q .; then
        echo "Unexpected package paths: $archive" >&2; exit 1
    fi
    tar -xf "$archive" -C "$base" --exclude=.PKGINFO --exclude=.BUILDINFO --exclude=.MTREE --exclude=.INSTALL
done < "$base/packages.list"
sha256sum "$base"/packages/*.pkg.tar.zst > "$base/packages.sha256"
echo "Isolated CLANG64 ready: $base/clang64"
