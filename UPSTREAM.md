# Upstream provenance

The current gaze source is a standalone OpenXR headless bridge. No custom
WiVRn export patch or xrizer modification is required. The WiVRn pin below is
retained only for the historical server build scripts.

## WiVRn

Repository: https://github.com/WiVRn/WiVRn

This project is pinned to the **same WiVRn release used by the current headset client**:

```text
release: v26.9
commit:  bbc6e4cc36c355fa6180980abd231673dc15115d
```

As of October 4, 2026, GitHub lists `v26.9` as the latest WiVRn release, and that tag points to the `bbc6e4c` commit used by this experiment.

**Important:** WiVRn requires the VR client and PC server to use the same version. This repository therefore refuses to build the WiVRn side from a different commit.

The WiVRn build in this repository is **PC/server-only**. `WIVRN_BUILD_CLIENT=OFF` is intentional: users should use the official matching headset client rather than being asked to build the Android app. WiVRn's own build system supports a server-only configuration.

The local working checkout also contained a `changes.patch` alongside the edited `server/compositor/foveation.cpp`; the repository's WiVRn patch is the same existing change supplied for this project.

## DXVK

Repository: https://github.com/doitsujin/dxvk

The patch targets `d30be2baea02a67b9502bffda0ec8b0d915bb3d2`, recorded in
`versions.env`. A clean application to that revision and the 64-bit Windows
build have been verified locally. The original VRS patch has been updated to
read the versioned OpenXR bridge protocol instead of WiVRn's export format.

The patch also includes the VRS tile-capacity fix from local DXVK commit
`1e25cb242d23be9f5455dca11d8a0761d1c6daab`. Attachment capacity scales with
the GPU's texel size to cover at least 8192 pixels per axis, subject to the
device's image-size limit. Image allocation, aligned upload-buffer allocation
and pass bounds checks use the same capacity, which is logged at startup.

The patch also ports double-buffered uploads and matching DOT latency from
canonical DXVK commit `985e429f25ca8c4ac70927f119df66f90e567178`
(`DXVK_GAZE_COMMIT` in `versions.env`). `DXVK_COMMIT` remains the upstream
patch-application base, so existing setup and patch application remain valid.

The VRS attachment and upload buffer now use two slots. Rendering keeps the
completed front image while an upload is pending in the back slot. GPU command
list completion is polled without waiting; a retired front slot is reused only
after all its recorded reads finish. The upload-to-attachment barrier is emitted
when publishing a completed image, before recording the next upload. Startup or
an incompatible render-target change uses full-rate rendering until a matching
image is ready. Updates may therefore lag by multiple frames when the GPU queue
is backed up; this is not a fixed one-frame delay. CPU tile generation and GPU
transfer work still occur, so live VRChat FPS improvement remains unmeasured.

`DXVK_GAZE_DOT=1` uses the gaze coordinates saved with the displayed attachment,
including upload latency and tile-granularity caching. The existing SBS-only DOT
and `DXVK_GAZE_FAKE=grid` overlay behavior is retained. Slot lifetime is tracked
by command lists, including teardown and partially failed resource allocation.

The working DXVK build configuration was:

```text
-Dbuildtype=release -Denable_d3d8=false -Denable_d3d9=false -Denable_d3d10=false --cross-file=build-win64.txt
```

The patch contains the parent blob hashes for the modified DXVK files.
