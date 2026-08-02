#!/usr/bin/env bash
set -euo pipefail

project_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
build_directory="${HEARTSTEAD_BUILD_DIR:-$project_root/build-linux}"
game_exe="$build_directory/heartstead"

echo "Checking for Heartstead updates..."
bash "$project_root/scripts/build-linux.sh" --no-tests

cd "$project_root"
exec "$game_exe"
