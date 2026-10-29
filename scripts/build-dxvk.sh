#!/usr/bin/env bash
set -euo pipefail

repo_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
source_dir="${DXVK_SOURCE_DIR:-$repo_dir/.work/dxvk}"
out_dir="${DXVK_OUTPUT_DIR:-$repo_dir/dist/dxvk}"

if [[ ! -d "$source_dir/.git" ]]; then
  echo "DXVK source not found: $source_dir" >&2
  echo "Clone the exact upstream revision used for your working tree, then set DXVK_SOURCE_DIR." >&2
  exit 1
fi

cd "$source_dir"

setup_args=()
if [[ -f build/meson-private/coredata.dat ]]; then
  setup_args+=(--reconfigure)
fi
meson setup "${setup_args[@]}" build \
  --cross-file build-win64.txt \
  -Dbuildtype=release \
  -Denable_d3d8=false \
  -Denable_d3d9=false \
  -Denable_d3d10=false
ninja -C build -j "${DXVK_BUILD_JOBS:-4}"

mkdir -p "$out_dir"
cp build/src/d3d11/d3d11.dll "$out_dir/"
cp build/src/dxgi/dxgi.dll "$out_dir/"
printf '%s\n' "Built:" "$out_dir/d3d11.dll" "$out_dir/dxgi.dll"
