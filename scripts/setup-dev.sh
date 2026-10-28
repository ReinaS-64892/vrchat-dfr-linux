#!/usr/bin/env bash
set -euo pipefail
repo_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
source "$repo_dir/versions.env"
source_dir="${DXVK_SOURCE_DIR:-$repo_dir/.work/dxvk}"

for tool in git meson ninja x86_64-w64-mingw32-g++ glslangValidator c++ pkg-config; do
  if ! command -v "$tool" >/dev/null; then
    echo "Missing tool: $tool (Arch: base-devel git meson ninja mingw-w64-gcc glslang openxr)" >&2
    exit 1
  fi
done
pkg-config --exists openxr

if [[ ! -e "$source_dir" ]]; then
  mkdir -p "$(dirname "$source_dir")"
  git clone https://github.com/doitsujin/dxvk.git "$source_dir"
  git -C "$source_dir" checkout --detach "$DXVK_COMMIT"
fi
actual="$(git -C "$source_dir" rev-parse HEAD)"
if [[ "$actual" != "$DXVK_COMMIT" ]]; then
  echo "DXVK must be at $DXVK_COMMIT; found $actual. Existing checkout left untouched." >&2
  exit 1
fi
git -C "$source_dir" submodule update --init --recursive
patch_file="$repo_dir/patches/dxvk-gaze-vrs.patch"
if git -C "$source_dir" apply --reverse --check "$patch_file" 2>/dev/null; then
  echo "DXVK gaze patch already applied."
elif git -C "$source_dir" apply --check "$patch_file"; then
  git -C "$source_dir" apply "$patch_file"
else
  echo "DXVK patch does not apply cleanly. Existing checkout left untouched." >&2
  exit 1
fi
bash "$repo_dir/scripts/build-openxr-probe.sh"
bash "$repo_dir/scripts/build-openxr-bridge.sh"
echo "Ready. Build DXVK with: DXVK_SOURCE_DIR='$source_dir' bash '$repo_dir/scripts/build-dxvk.sh'"
