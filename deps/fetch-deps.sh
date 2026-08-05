#!/usr/bin/env bash
# Fetch build dependencies that are not vendored:
#   deps/SDL      — SDL2 source (Java shell + libSDL2.so), pinned tag
#   deps/libnode  — prebuilt android-aarch64 libnode.a + headers
# wasmcart-native is a git submodule, not fetched here.
set -euo pipefail
cd "$(dirname "$0")"

SDL_TAG="${SDL_TAG:-release-2.32.10}"
LIBNODE_RELEASE="${LIBNODE_RELEASE:-v26.3.0-jsg9}"

if [ ! -d SDL ]; then
    echo "fetching SDL ${SDL_TAG}..."
    git clone --depth 1 --branch "${SDL_TAG}" https://github.com/libsdl-org/SDL.git SDL
fi

if [ ! -f libnode/libnode.a ]; then
    echo "fetching libnode ${LIBNODE_RELEASE} (android-aarch64)..."
    mkdir -p libnode
    curl -fsSL "https://github.com/wasmcart/build-libnode/releases/download/${LIBNODE_RELEASE}/libnode-android-aarch64.tar.gz" \
        | tar xz -C libnode
fi

echo "deps ready:"
echo "  SDL:     $(git -C SDL describe --tags 2>/dev/null || echo '?')"
echo "  libnode: $(cat libnode/NODE_VERSION 2>/dev/null || echo '?')"
