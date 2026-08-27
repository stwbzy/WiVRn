# Current fork handoff

Status date: 2026-08-27.

Release status: the final physical Quest 3S pass completed without additional
errors. The stable package target is `v0.3.0-fsr-runtime`; native 1:1, Meta
compositor/SGSR scaling and supervised FBT-E are release paths. Color-only
SSW/ASW remains opt-in, Off by default and explicitly experimental.

## Checkout and revisions

The active checkout and branch are:

```text
/home/stwby/Proyectos/WiVRn-fsr
feature/fsr-frame-generation
```

The release work started from `ee66168032a09b8e3f79214eb5437515e1a495ad`
(`Allow FSR to sample the squasher UNORM view`). The Quest compatibility,
runtime SSW/ASW, lifecycle and FBT-E changes are collected by the
`v0.3.0-fsr-runtime` release commit and tag.

## Build and installed-client status

The current server builds at:

```text
build-server-no-system-boost/server/wivrn-server
```

The matching Android artifacts are:

```text
build/outputs/apk/debug/WiVRn-fsr-debug.apk
build/outputs/apk/release/WiVRn-fsr-release-unsigned.apk
```

The debug APK was rebuilt and installed on the connected Quest 3S as
`org.meumeu.wivrn.local`. The release APK remains unsigned. After the latest
server changes the complete local suite passes:

```text
connection-lifecycle  passed
scaling-watchdog      passed
fsr-config            passed
```

Build and verify with:

```bash
cmake --build build-server-no-system-boost -j4 --target wivrn-server
ctest --test-dir build-server-no-system-boost --output-on-failure

ANDROID_HOME=/home/stwby/Android ./gradlew assembleDebug
adb install -r -d build/outputs/apk/debug/WiVRn-fsr-debug.apk
```

## Implemented graphics paths

- Native 1:1 remains the stable baseline.
- Meta compositor scaling separates game/stream render resolution from the
  OpenXR target-layer resolution. Horizon OS performs the final panel scale.
- Mobile SGSR1 uses the Qualcomm-style directionless reconstruction in the
  client defoveation pass and avoids per-sample `pow()` conversion.
- FSR1 EASU/RCAS remains available on the Linux server.
- NIS configuration remains compatible and falls back safely while no
  portable backend is available.
- Bilinear target scaling is fused with client defoveation.
- Reconstruction is skipped below the 10% per-dimension threshold.
- The 180-frame scaling watchdog uses client-pass p95 and missed intervals and
  never overwrites the saved preference.

On the Quest 3S used for development, Meta compositor scaling worked but was
subjectively more pixelated than SGSR1 at an aggressive 80% render to 119%
target configuration. This is an observation, not a completed quality result.
OpenXR permits WiVRn to select normal/quality/automatic compositor filtering
where supported, but it does not expose Horizon's internal upscaler or promise
SGSR-equivalent reconstruction.

## Server runtime ASW status

The obsolete `fps_divider` behavior has been replaced by Off/Auto/Always
server-side frame synthesis. It does not use `XR_FB_space_warp`, does not
modify VRChat, and does not require game depth or motion vectors.

Implemented:

- previous/current real-frame history;
- portable hierarchical Vulkan optical flow at 1/16 and 1/4 resolution;
- confidence-gated extrapolation for a half-period interval;
- half-rate application pacing and full-rate encode/transport;
- telemetry for real/synthetic frames, optical-flow cost, missed intervals and
  fallback reason;
- session-only optical-flow and encode/network deadline watchdogs;
- `WIVRN_DISABLE_FRAME_SYNTHESIS=1` diagnostic kill switch;
- temporal/pacer reset when ASW cadence changes;
- overlay-only protection so WayVR feedback cannot disable ASW while no stereo
  projection layer exists.

Physical Quest 3S validation at 72 Hz reached the real backend:

```text
application pacing 36Hz, encode/transport 72Hz
projection_layers=1 projection_views=2 inputs_ready=1
hierarchical-flow views=2 extent=1465x1535 extrapolation=0.50
backend_dispatches=182
```

The test then produced missing video shards, stale headset frames and three
failed encode/network feedback windows. The safety path changed application
pacing back to 72 Hz and logged:

```text
Server ASW disabled until reconnect: reason=encode_or_network_deadline
```

The connection remained streaming, D-Bus `DiscardedPackets` remained zero,
and the Quest returned to 72/72 with no sustained stale frames or black screen.
Therefore `Always` is not release-ready at the tested 10-bit, 50 Mbit/s,
1465x1535-per-eye load. Use Off for the stable baseline and Auto only for
continued experimental testing.

An earlier build showed a cream/purple right-eye tint in Always mode. The
current controlled run entered ASW and then fell back too quickly to close
that visual issue, so stereo color correctness remains an open acceptance
item.

## Fixed during the 2026-08-13 physical debug session

### Feedback generation mismatch

The normal network sender stamped `connection_generation`, but the display
feedback batch in `client/scenes/stream.cpp` bypassed it. This generated about
144 mismatched feedback packets per second and could inflate latency to
150-250 ms. Both feedback entries now carry the active network generation.
The server also accepts and restamps display feedback received over the
authenticated per-generation control channel; tracking, input and UDP remain
strict.

After installing the matching APK, the mismatch counter stayed at zero.
With ASW Off, the measured physical baseline was:

```text
WayVR:  50-60 ms
VRChat: 69-83 ms, usually about 71 ms
```

The earlier VRChat head-motion vibration was no longer visible.

### Runtime and process-start races

- Headset connections are not accepted until the D-Bus service and child
  manager are initialized.
- A per-instance `flock` prevents a second server from unlinking the live
  compositor socket.
- `--early-active-runtime` now creates the active runtime only after obtaining
  that lock. Previously a rejected second invocation could construct and then
  destroy its runtime setter, deleting the first server's
  `~/.config/openxr/1/active_runtime.json`.

The regression test started a second server while one was active. It exited
with `Another WiVRn server instance is already running` and the OpenXR runtime
manifest retained the same inode, size and target.

### XRizer/VRChat desktop-mode diagnosis

The reported “XRizer crash” was not a crash. VRChat logged
`OpenVR Init_InterfaceNotFound` and fell back to desktop because the active
OpenXR runtime file had been removed by the race above. There was no new
VRChat, XRizer or WiVRn coredump. Restoring the build-tree manifest and
relaunching produced:

```text
StartVRSDK: OpenVRLoader
[SteamVR] Initialized
application_name: 'VRChat'
```

VRChat then submitted a stereo projection layer and exercised ASW.

## Reproduction commands

Start the current server from the build tree:

```bash
cd /home/stwby/Proyectos/WiVRn-fsr
./build-server-no-system-boost/server/wivrn-server --early-active-runtime
```

For foreground server debugging, add `--no-fork`. Do not start another server
while a session is active; the new binary rejects it safely, but mixed old/new
processes make logs ambiguous.

For the locally installed WayVR whose Monado client tag differs from this
build-tree runtime:

```bash
env \
  XR_RUNTIME_JSON="$PWD/build-server-no-system-boost/openxr_wivrn-dev.json" \
  IPC_IGNORE_VERSION=1 \
  wayvr --openxr
```

Use `IPC_IGNORE_VERSION=1` only when intentionally testing compatible local
artifacts. It must not hide an actual IPC layout mismatch.

Useful live checks:

```bash
pgrep -af 'wivrn-server|VRChat.exe|wayvr'
busctl --user introspect io.github.wivrn.Server /io/github/wivrn/Server
adb logcat -v threadtime | rg -i 'WiVRn|VrApi|FrameSync|Drop shard|not sent'
```

## Post-release experimental coverage

1. Expand Quest 2 coverage at 72/80/90 Hz and the Quest 3S 90 Hz matrix.
2. Compare Meta compositor filters against SGSR1 with matched source and target
   resolutions; do not claim quality parity from the earlier 80->119 test.
3. Extend continuous SSW/ASW visual coverage across right-eye color,
   head/hand/avatar motion, menus, particles and scene cuts.
4. Test Intel, AMD and NVIDIA Vulkan/encoder combinations plus prolonged
   adverse-network recovery.

Detailed behavior lives in [connection-lifecycle.md](connection-lifecycle.md),
graphics and ASW behavior in
[fsr-frame-generation.md](fsr-frame-generation.md), and operator diagnostics
in [debugging.md](debugging.md).
