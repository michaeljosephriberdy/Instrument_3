#!/bin/bash
# fix_build.sh
# Fixes: "Error: could not load cache"
#
# Root cause: audio.sh deletes CMakeCache.txt from the project root but
# generates the fresh cache inside build/ (via `cmake -B build -S .`).
# Running `cmake --build .` from the project root then finds no cache there.
#
# This script rebuilds correctly against build/, and does NOT touch any
# source files (see warning about clean_structure.txt in the chat).

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$REPO_ROOT"

echo "=== Build Fix ==="
echo "Project root: $REPO_ROOT"

if [ ! -f "build/CMakeCache.txt" ]; then
    echo "No cache found in build/ -- regenerating it."
    cmake -B build -S . -DCMAKE_BUILD_TYPE=Release -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
else
    echo "Found existing cache at build/CMakeCache.txt."
fi

echo "Building against the build/ directory (not the project root)..."
cmake --build build --clean-first

echo ""
echo "=== Done ==="
echo "Binary should be at: build/microtonal_instrument"
echo "Run it with: sudo ./build/microtonal_instrument"
