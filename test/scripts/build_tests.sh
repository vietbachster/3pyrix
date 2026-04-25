#!/bin/bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TEST_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="$TEST_DIR/build"

mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"

cmake "$TEST_DIR" -DCMAKE_BUILD_TYPE=Debug
cmake --build . --parallel
