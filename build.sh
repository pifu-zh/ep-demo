#!/bin/bash
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

BUILD_DIR="$SCRIPT_DIR/build"

if [ ! -d "$BUILD_DIR" ]; then
    mkdir -p "$BUILD_DIR"
fi

cd "$BUILD_DIR"
cmake ..
make -j$(nproc)

echo ""
echo "Build complete: $BUILD_DIR/test_init"
