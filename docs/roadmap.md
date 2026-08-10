# FSR 1 spatial upscaling and connection-stability roadmap

Status date: 2026-08-10.

The fork keeps VRChat, OpenComposite, and game binaries unmodified. The
runtime/compositor owns the opt-in FSR 1 spatial path. Connection lifecycle
work applies generally to supported headsets and applications.

## Completed: structural frame-synthesis path

- Created the `feature/fsr-frame-generation` branch and reproducible
  `server-no-system-boost` build.
- Added Monado patches `0010`, `0011`, and `0012` for opt-in
  `XR_EXT_frame_synthesis` and `XR_FB_space_warp` validation and XRT/IPC
  transport of stereo color, motion-vector, and depth swapchains.
- Forwarded the new metadata through the GL, Vulkan, and multi-client
  compositor layers.
- Added the guarded WiVRn compositor probe and the standalone Vulkan/OpenXR
  `wivrn-fsr-openxr-harness`.
- Confirmed extension advertisement and stereo metadata delivery with
  `views=2`, two motion-vector views, two depth views, and `inputs_ready=1`.
- Completed a 300-frame physical smoke test and a 20,000-frame structural run
  across a voluntary disconnect/resume.
- Preserved normal composition whenever an experimental prerequisite is
  missing.

The original structural-only probe reported:

```text
backend=none fallback=normal-composition
```

That limitation has now been superseded locally by the experimental backend
described below.

## Completed: deterministic connection lifecycle

- Explicit server/client states, close reasons, generations, transition
  duration, counters, and D-Bus properties.
- `HeadsetConnected=true` only after the complete handshake.
- Bidirectional `disconnect_request`/`disconnect_ack` and
  `wivrnctl disconnect --mode default|pause|terminate`.
- Public `disconnect-policy`, defaulting to `pause`.
- Idempotent compositor/audio pause and resume, temporal reset, decoder and
  `AImageReader` recreation, fresh PipeWire streams, generation filtering,
  heartbeats, and IDR request.
- Thirty-second unexpected-loss window with the specified headset backoff.
- Resume authorization using the paired headset key and a private persistent
  session token, while permitting IP-address changes.
- Strict resume compatibility checks for codec, bit depth, resolution, audio,
  FOV, refresh rates, and headset features.
- Local lobby/countdown instead of a stale streamed frame.
- Bounded handling of transient Android `xrWaitFrame` failures and clean local
  XR-activity recreation for the persistent Meta runtime failure.
- Deterministic unit coverage including 100 pause/resume cycles and a TCP/UDP
  fault proxy for integration work.

See [connection-lifecycle.md](connection-lifecycle.md) for the normative
behavior and [debugging.md](debugging.md#connection-lifecycle) for diagnostics.

## Deferred: experimental frame-generation backend

- The earlier persistent stereo history, optical-flow candidate and 45-to-90
  transport remain isolated in source for future repair.
- The default CMake build sets
  `WIVRN_ENABLE_EXPERIMENTAL_FRAME_GENERATION=OFF`; stale runtime variables
  cannot activate the path.
- Before re-enabling it, fix image-layout ownership, candidate lifetime,
  double-rate pacing, decoder timestamps and reconnect reset ordering.

## Completed locally: stable FSR 1 spatial path

- Added a 32-bit Vulkan FSR 1 EASU pass followed by RCAS before WiVRn
  foveation/encoding.
- Added explicit negotiation of the headset's unscaled recommended resolution.
- Kept FSR spatial processing stateless across pause and reconnect.
- Built the server and matching Android debug APK, validated both FSR shaders
  as Vulkan 1.3 SPIR-V, verified APK v2 signing, and passed both local tests
  (`connection-lifecycle` and `fsr-config`).

This backend is not AMD FidelityFX FSR 3. Spatial upscaling uses AMD's
open-source FSR 1 EASU/RCAS algorithm. The deferred frame-generation code is
not part of the default runtime.

The active FSR path should produce a marker equivalent to:

```text
FSR 1 spatial upscaling enabled: EASU ... -> ... per eye, RCAS sharpness=... stops
```

## Current phase: physical revalidation and hardening

1. Install the matching APK and compare the FSR-only path against native
   resolution on Quest without changing application cadence.
2. Automated fault-proxy matrix: TCP-only, UDP-only, combined cuts, delay,
   handshake closure, address change, incompatible headset, and server shutdown
   from every state.
3. Visual comparison with FSR 1 disabled and enabled, including fine text,
   rotation, translation, disocclusion and transparent content.
4. VRChat smoke tests without game-specific hooks or binary changes.
5. Add counters for generated frames, backend fallbacks and pacing misses.
6. Physical release gate: two hours, twenty Wi-Fi cuts, twenty voluntary
   disconnects, no native crash, orphan thread, retained lock, or persistent
   MediaCodec failure.
7. Revisit the frame-generation backend only after the FSR-only path passes the
   stability gate and the dedicated synthesis tests are restored.

## Safety constraints

- Never replace a submitted frame with an incomplete generated image.
- Never use stale frames, poses, packets, or temporal history from an older
  connection generation.
- Do not describe FSR 1 spatial reconstruction or the color-only residual field
  as AMD FSR 3, real application motion, or real scene depth.
- Do not bypass IPC version checks between unrelated installed/build-tree
  runtimes.
- Keep normal composition available for every failure.
