#!/usr/bin/env bash
set -euo pipefail
repo_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
mkdir -p "$repo_dir/build"
"${CXX:-c++}" -std=c++17 -O2 -Wall -Wextra -Wno-missing-field-initializers \
  "$repo_dir/tools/openxr-gaze-probe.cpp" \
  $(pkg-config --cflags --libs openxr) -pthread -o "$repo_dir/build/openxr-gaze-probe"
echo "Built: $repo_dir/build/openxr-gaze-probe"
