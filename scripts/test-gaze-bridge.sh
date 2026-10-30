#!/usr/bin/env bash
set -euo pipefail
repo_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
mkdir -p "$repo_dir/build"
python3 - "$repo_dir" <<'PY'
from pathlib import Path
import sys
root = Path(sys.argv[1])
patch = (root / 'patches/dxvk-gaze-vrs.patch').read_text()
section = patch.split('+++ b/src/dxvk/dxvk_gaze_shm.h\n', 1)[1].split('diff --git ', 1)[0]
embedded = ''.join(line[1:] + '\n' for line in section.splitlines() if line.startswith('+'))
assert embedded == (root / 'shared/gaze_shm.h').read_text(), 'DXVK protocol header differs from canonical ABI'
for name in ('dxvk_gaze_buffer.h', 'dxvk_gaze_map.h'):
    section = patch.split(f'+++ b/src/dxvk/{name}\n', 1)[1].split('diff --git ', 1)[0]
    header = ''.join(line[1:] + '\n' for line in section.splitlines() if line.startswith('+'))
    (root / 'build' / name).write_text(header)
PY
common=(-std=c++17 -O2 -Wall -Wextra -Wno-missing-field-initializers)
"${CXX:-c++}" "${common[@]}" "$repo_dir/tests/gaze_unit.cpp" -o "$repo_dir/build/gaze-unit"
"$repo_dir/build/gaze-unit"
"${CXX:-c++}" "${common[@]}" -I"$repo_dir/build" "$repo_dir/tests/gaze_buffer_unit.cpp" -o "$repo_dir/build/gaze-buffer-unit"
"$repo_dir/build/gaze-buffer-unit"
"${CXX:-c++}" "${common[@]}" -pthread -I"$repo_dir/build" "$repo_dir/tests/gaze_map_unit.cpp" -o "$repo_dir/build/gaze-map-unit"
"$repo_dir/build/gaze-map-unit"
"${CXX:-c++}" "${common[@]}" "$repo_dir/tools/openxr-gaze-bridge.cpp" \
  "$repo_dir/tests/fake_openxr.cpp" -pthread -o "$repo_dir/build/gaze-bridge-fake"
python3 "$repo_dir/tests/test_gaze_bridge.py" "$repo_dir/build/gaze-bridge-fake"
