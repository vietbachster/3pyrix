#!/bin/bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TEST_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
BIN_DIR="$TEST_DIR/build/bin"

if [ ! -d "$BIN_DIR" ]; then
  echo "Build directory not found. Run build_tests.sh first."
  exit 1
fi

FAILED=0

for test_exe in "$BIN_DIR"/*; do
  if [ -x "$test_exe" ] && [ -f "$test_exe" ]; then
    echo "----------------------------------------"
    echo "Running: $(basename "$test_exe")"
    echo "----------------------------------------"
    if ! "$test_exe"; then
      FAILED=1
    fi
    echo ""
  fi
done

exit "$FAILED"
