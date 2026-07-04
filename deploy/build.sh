#!/usr/bin/env bash
# build.sh — rebuild belltracker (no apt; deps assumed installed via setup.sh)
set -e
cd "$(dirname "$0")/.."          # → ~/Belltracker
mkdir -p build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j"$(nproc)"
echo "Built: $(pwd)/belltracker"
