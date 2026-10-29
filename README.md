# WiVRn + DXVK Gaze-Driven VRS

Experimental gaze-driven variable rate shading for VRChat over WiVRn(also works for any DXVK application).
Tested on Arch with a Pico 4 Pro.

A separate native Linux process reads gaze through standard OpenXR
(`XR_EXT_eye_gaze_interaction` with `XR_MND_headless`) and publishes per-eye
centers to `/dev/shm/vrchat_dfr_gaze`. Patched DXVK reads this shared memory
and builds a Vulkan fragment-shading-rate attachment around the gaze point.
WiVRn's gaze-export patch and changes to xrizer are not required.

The original WiVRn-export transport was tested on the hardware above. The new
OpenXR bridge has build and automated test coverage; live HMD / VRChat
validation is still pending.

VRChat issues several passes of similar size, so VRS has to be restricted
to the RGBA16F scene pass (`fmt=97`) with a Y flip - otherwise you get
multiple foveation spots. These are the tested values, not universal
requirements.

> [!WARNING]
> Experimental, use at your own risk. This is an unofficial modification, not endorsed by or affiliated with VRChat, WiVRn or DXVK. It replaces `d3d11.dll` and `dxgi.dll` in your Proton install, and VRChat's rules or anti-cheat may treat modified clients differently. I haven't verified how. Tested only on an RTX 3060 Ti with a Pico 4 Pro.  
> I also recommend against using MSAA/Anti-Aliasing in VRChat with this, as increasing the MSAA sample count can limit the maximum VRS coarse rate available on some GPUs. For better image quality, use a higher render resolution instead.

## Install

**DXVK** - replaces `d3d11.dll` and `dxgi.dll` in a Proton install:  
**! Replaces files in your Proton install (originals are backed up to `.backup/`).**

    ./scripts/install-dxvk.sh --dxvk-from ./dxvk-gaze-vrs.zip "/path/to/Proton"  


Omit `--dxvk-from` to use a local build from `dist/dxvk`. The installer
backs up the originals to `.backup/` and only touches those two files. A
Proton update overwrites them - reinstall afterward.

**Gaze bridge** — build and run on the Linux host:

```bash
# Arch dependencies: base-devel git meson ninja mingw-w64-gcc glslang openxr
bash scripts/setup-dev.sh
bash scripts/build-dxvk.sh

# Connect the HMD to your normal WiVRn server, with eye tracking enabled.
./dist/gaze/openxr-gaze-bridge --verbose
```

Keep the bridge running while playing. It reconnects to OpenXR every 2 seconds
when the runtime or HMD is unavailable. Ctrl-C stops it. A matching WiVRn
server/headset client is still required, but the server does not need the
custom gaze-export patch. If needed, set `XR_RUNTIME_JSON` to your WiVRn
OpenXR runtime manifest for the bridge process.

The bridge polls at 250 Hz. OpenXR provides one combined gaze ray; each eye's
pose and FOV are used to project it independently. The default assumes infinite
fixation distance. `--distance 1` instead assumes a fixation point 1 metre along
the ray; this is an approximation, not measured convergence. Use `--help` for
the polling rate, sample-age limit, output path and timed diagnostic options.

## Run VRChat

    DXVK_GAZE_VRS=1 DXVK_GAZE_FMT=97 DXVK_GAZE_FLIP_Y=1 DXVK_GAZE_FULL_AREA=45 DXVK_GAZE_MID_AREA=65 DXVK_GAZE_COARSE=auto

Add `DXVK_GAZE_DEBUG=1 DXVK_GAZE_DOT=1` to draw the gaze dots. See
[`config/vrchat.env`](config/vrchat.env).

Verify the shared-memory file exists while a session is up:

    ls -l /dev/shm/vrchat_dfr_gaze

The shared format is versioned and incompatible with the old `wivrn_gaze`
export. DXVK disables gaze VRS for invalid data or a heartbeat stopped for
100 ms. It waits for a sequence change before accepting a newly mapped file.
After a stale mapping is closed, reopening may take up to 2 seconds.

If Steam's container has a different `/dev/shm`, choose a directory visible
both to the host and Proton, for example a directory under your home:

```bash
mkdir -p "$HOME/.cache/vrchat-dfr"
./dist/gaze/openxr-gaze-bridge --file "$HOME/.cache/vrchat-dfr/gaze" --verbose
```

Set `DXVK_GAZE_FILE` to the corresponding **Wine path** in VRChat's launch
options, for example `'Z:\home\reina\.cache\vrchat-dfr\gaze'`. Both processes
must see the same file. The bridge preserves its inode across restarts; do not
remove or truncate it while a reader is running.

## Env vars

| Variable | Purpose |
|---|---|
| `DXVK_GAZE_VRS=1` | Enable gaze VRS |
| `DXVK_GAZE_FMT=97` | Restrict to the RGBA16F scene pass |
| `DXVK_GAZE_FLIP_Y=1` | Flip scene-pass Y |
| `DXVK_GAZE_FULL_AREA` | Percent of the eye image that renders at full rate |
| `DXVK_GAZE_MID_AREA` | Percent rendered at full or 2×2; beyond it uses `COARSE` |
| `DXVK_GAZE_R1` / `R2` | Inner / outer radius of the sharp region |
| `DXVK_GAZE_COARSE` | Outer shading-rate code (`auto` = Highest available for your system), you may also enter an int |
| `DXVK_GAZE_DEBUG=1` | Pass/gaze diagnostics |
| `DXVK_GAZE_DOT=1` | Draw diagnostic gaze dots |
| `DXVK_GAZE_SWAP_EYES=1` | Swap per-eye gaze data |
| `DXVK_GAZE_GAIN_Y` | Scale vertical gaze offset around center |
| `DXVK_GAZE_FLIPX_L/R`, `FLIPY_L/R` | Force per-eye X/Y flip |
| `DXVK_GAZE_FAKE=x/y/circle/fixed/grid` | Synthetic gaze modes |
| `DXVK_GAZE_SIZE=WxH`, `SAMPLES` | Force size / sample count |
| `DXVK_GAZE_FILE` | Override the gaze file path |

For a debug run with `PROTON_LOG=1`:

    grep -a "passes in last" -A8 ~/steam-438100.log | tail -24

### Choosing the sharp area

`DXVK_GAZE_FULL_AREA` and `DXVK_GAZE_MID_AREA` are percentages of one eye's
image. Keep `MID_AREA` above `FULL_AREA`. `100` means the whole eye. The circle's area also counts any part that hangs outside the image, so above
~78% the full-resolution part inside the image is smaller than the percentage
(the corners stay coarse). `100` means the whole eye.

`DXVK_GAZE_R1` / `R2` still work (radius in units of the shorter eye
dimension) and are used only when the area variables are not set.

## Build

The DXVK revision is pinned in `versions.env`. `setup-dev.sh` clones it into
`.work/dxvk`, initializes submodules, applies the patch if needed, and builds
both the bridge and the OpenXR probe. It leaves an existing different DXVK HEAD
untouched and reports an error.

```bash
bash scripts/setup-dev.sh
bash scripts/build-dxvk.sh         # dist/dxvk/{d3d11,dxgi}.dll
bash scripts/build-openxr-bridge.sh # dist/gaze/openxr-gaze-bridge
bash scripts/test-gaze-bridge.sh    # projection, IPC and simulated OpenXR tests
```

`DXVK_SOURCE_DIR`, `DXVK_OUTPUT_DIR` and `DXVK_BUILD_JOBS` override the DXVK
source path, output path and parallel job count (default 4). `CXX` and
`GAZE_OUTPUT_DIR` override the bridge compiler and output directory.

For extension enumeration without opening a session, use
`./build/openxr-gaze-probe`. Add `--seconds 15` to that probe to inspect raw
tracking state. For the new publisher, `./dist/gaze/openxr-gaze-bridge
--seconds 15 --verbose` exercises gaze-to-UV conversion and shared-memory output.
Both timed commands return 1 if no valid sample was observed.

The original `build-wivrn.sh`, `apply-wivrn.sh` and
`patches/wivrn-gaze-export.patch` are retained as historical tooling and are
not part of this build or the current data path.

See [architecture](docs/architecture.md) for the protocol and failure behavior,
and [the investigation](docs/eye-tracking-investigation.md) for the alternative
OpenVR / existing-session route and its unresolved integration questions.

## Architecture

    Eye tracking
          |
          v
    Unmodified WiVRn OpenXR runtime
          |
          v
    openxr-gaze-bridge (Linux headless process)
          |
          v
    /dev/shm/vrchat_dfr_gaze (versioned shared memory)
          |
          v
    DXVK (via Wine path)
          |
          v
    VK_KHR_fragment_shading_rate
          |
          v
     VRChat scene pass

## Notes

- The DXVK patch now uses the OpenXR bridge protocol. The WiVRn patch is retained
  for historical reference only.
- Everything here is experimental; `fmt=97` and `FLIP_Y=1` are just the
  values that worked for this test.
- Revisions: DXVK `d30be2ba`, WiVRn `v26.9` / `bbc6e4c` (see
  `versions.env`). The DXVK patch records its parent blob hashes, so the
  DXVK ref must match exactly.

## Demo with overexadurated values and gaze dot enabled
[Google drive link](https://drive.google.com/file/d/1OabrqSbzjEp96AFs6StbJrUAcHWSyMiQ/view?usp=sharing)
