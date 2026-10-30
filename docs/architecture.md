# OpenXR gaze bridge architecture

## Data path

`openxr-gaze-bridge` is a native Linux process with its own OpenXR instance and
headless session. It enables `XR_MND_headless`, `XR_EXT_eye_gaze_interaction`
and `XR_KHR_convert_timespec_time`, binds
`/user/eyes_ext/input/gaze_ext/pose`, polls session events and synchronizes its
own action set. It never creates a swapchain or calls frame submission APIs.

At each tick (default 250 Hz), it converts `CLOCK_MONOTONIC` to `XrTime`, locates
gaze relative to VIEW, and obtains stereo view poses/FOVs with `xrLocateViews`
at the same time. This is current host time, not VRChat's predicted display
time. Unknown sample timestamps are allowed; known samples older than 100 ms
are rejected by default (`--max-age-ms` changes this limit).

The combined ray is transformed into each eye's coordinates, including eye
rotation. By default it is projected at infinity. With `--distance N`, a point
N metres along the ray is assumed and eye translation is included. Neither
option supplies measured binocular convergence. Projection uses tangents of
each eye's asymmetric FOV and produces UVs with top=0. Finite UVs are clamped
to image bounds. Invalid poses, FOVs and rays behind an eye invalidate gaze.

The default output is `/dev/shm/vrchat_dfr_gaze`. DXVK maps it through Wine as
`Z:\dev\shm\vrchat_dfr_gaze`; `--file` and `DXVK_GAZE_FILE` override the host
and Wine paths. Containers must expose the same file to both processes.
The reader does not load OpenVR/OpenXR or depend on xrizer changes. WiVRn's
normal OpenXR interface supplies data without its gaze-export patch.

## Shared memory protocol

The canonical ABI is [shared/gaze_shm.h](../shared/gaze_shm.h), embedded unchanged
as `src/dxvk/dxvk_gaze_shm.h` in the DXVK patch. It is 64 bytes, little endian,
with 16 aligned 32-bit words. Linux GCC/Clang and MinGW use lock-free 32-bit
atomic operations for every word, including payload. There is no interprocess
C++ mutex or non-atomic concurrent payload access.

| Word | Contents |
| --- | --- |
| 0 | Magic `0x325a4147` (`GAZ2`) |
| 1 | ABI version 1 |
| 2 | Size 64 |
| 3 | Sequence, odd during write, even when stable |
| 4 | Valid, exactly 1 for valid gaze |
| 5–6 | Left and right U, IEEE 754 float bits |
| 7–8 | Left and right V, IEEE 754 float bits |
| 9–15 | Reserved, zero |

A single writer holds an exclusive `flock` for the mapping's lifetime. It
publishes odd sequence, header/payload, then even sequence, using sequentially
consistent atomic operations. The reader accepts a packet only if the sequence
is even and unchanged across atomic payload loads. It validates magic, version,
size, finite coordinates and range. Readers retry interrupted snapshots on the
next worker tick; they never block waiting for a writer.

The output is a regular file owned by the current user, mode 0600. Existing
files must have size zero or exactly 64. The writer keeps the inode and
sequence across restarts, rather than unlinking/replacing it. It repairs an odd
sequence left by a killed writer. Symlinks and simultaneous writers are
rejected. The old WiVRn `GAZE` format is incompatible and not accepted.

## Failure and restart behavior

The bridge publishes invalid packets while waiting for READY, while actions are
inactive/not focused, when tracking is invalid/untracked, or when projection is
invalid. OpenXR failures invalidate output before destroying the session,
then retry initialization after 2 seconds. SIGINT/SIGTERM invalidate output
before closing the connection. No valid center is held across tracking loss.

DXVK's `Freshness` requires a sequence change after opening a mapping, so an
abandoned file is not accepted at reader startup. An unchanged sequence expires
after 100 ms measured with the reader's steady clock. This handles SIGKILL, a
stalled API call and a crash during an odd sequence without comparing Wine and
Linux clocks. Invalid packets disable VRS when observed. A stale mapping is
closed; opening is throttled to once every 2 seconds, so replaced files can
eventually be discovered as well.

The sample-age limit and heartbeat timeout are independent. If the runtime
supplies no sample timestamp, sensor sample age cannot be checked, but tracking
flags and heartbeat still apply. `--verbose` reports unknown timestamps and
the reason a packet is invalid.

## VRS and render coordinates

The existing shading-rate attachment generation and pass selection remain in
DXVK. Depth-only passes are rejected; format and sample-count filters can
restrict scene passes. The original VRChat settings use format 97 (RGBA16F)
and `DXVK_GAZE_FLIP_Y=1`. Global/per-eye flips and eye swapping remain available.
This transport sets source flip flags to zero.

Runtime FOVs can differ from an application's custom projection or submitted
texture bounds. Gaze-dot alignment in VRChat must be checked on hardware;
automated tests establish coordinate math and protocol behavior, not VRChat's
projection conventions or end-to-end latency.

## DXVK worker and GPU buffers

`dxvk-cs` selects eligible render passes and sends their dimensions, layer
count, format, sample count, VRS texel size and rate/radius settings to
`dxvk-gaze-map`. The worker polls the shared gaze source every 4 ms after its
first request. File opening/mapping, heartbeat checks, coordinate transforms,
fake gaze, map allocation and full tile traversal run on this worker.

The mailbox holds one queued request and one completed result. New requests
replace queued work; an in-progress map finishes before the next request is
handled. A try-lock lets the render thread skip the exchange under contention.
CPU map ownership is swapped and storage is recycled by the worker. A stationary
or tile-quantized unchanged gaze does not cause repeated map generation.

The render thread records uploads only for completed maps compatible with the
current pass. Two GPU image/upload-buffer slots retain the previous completed
attachment while an upload is pending. A slot is overwritten only after all
its recorded GPU uses finish; completion is polled without waiting. The upload
and its command recording remain on the graphics path. Startup and target
changes use full-rate rendering until a compatible map is ready. Invalid gaze
or worker failure disables VRS. Debug DOT uses the gaze saved with the displayed
GPU attachment, including worker and upload latency.

`dxvk_gaze_source.*` owns the CPU gaze adapter, `dxvk_gaze_map.h` contains the
map algorithm and mailbox/worker, and `dxvk_gaze_vrs.*` handles Vulkan integration.
The worker implementation is private to the Vulkan integration source file so
CPU changes do not require exposing worker details in the rendering header.

## Validation

`bash scripts/test-gaze-bridge.sh` checks symmetric/asymmetric FOVs, canted eyes,
upward gaze, finite fixation distance, malformed inputs, ABI validation,
sequence wrap, stale-file rejection and process-shared packet consistency.
The real publisher is also linked against a simulated OpenXR implementation to
test sampling, invalid tracking/views, inactive/unfocused actions, sample age,
missing extensions, runtime restart, session loss, output locking, inode
preservation, SIGTERM invalidation and SIGKILL sequence freeze.

The same script also checks two-slot GPU ownership policy and the CPU map
worker: map contents, alignment padding, latest-request replacement, mailbox
contention, independent polling, invalidation/recovery and failure handling.
Canonical DXVK additionally has a native source-adapter test for fake gaze,
coordinate transforms and fixed-gaze caching.

The patched Windows DLLs and Linux bridge build locally. Live HMD sampling,
concurrent VRChat sessions, Wine/container file visibility, gaze-dot alignment
and latency still require hardware validation.
