#!/usr/bin/env bash
set -euo pipefail

project_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
build_directory="${HEARTSTEAD_BUILD_DIR:-$project_root/build-linux}"

cmake \
    -S "$project_root" \
    -B "$build_directory" \
    -DCMAKE_BUILD_TYPE=Release
cmake --build "$build_directory" --parallel

if [[ "${1:-}" == "--no-tests" ]]; then
    exit 0
fi

ctest --test-dir "$build_directory" --output-on-failure
