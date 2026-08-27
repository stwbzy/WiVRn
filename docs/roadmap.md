# FSR 1 spatial upscaling and connection-stability roadmap

Status date: 2026-08-13.

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

The structural `XR_FB_space_warp` work is validation and transport scaffolding,
not evidence that WiVRn can enable AppSW for arbitrary streamed games. AppSW
still requires semantically correct application depth and motion vectors. The
separate client-side, color-only SSW-like contract is documented in
[client-side-ssw.md](client-side-ssw.md); it does not depend on a private
Virtual Desktop interface.

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

## Pre-release: Quest compatibility and runtime ASW

- Meta compositor scaling separates render/stream resolution from the OpenXR
  layer target and is recommended on Quest 1/2.
- Lightweight SGSR1 and fused bilinear run in the existing client defoveation
  pass, with a 180-frame safety watchdog.
- Experimental Off/Auto/Always runtime ASW uses the explicitly selected
  `server` backend when requested: it uses hierarchical optical flow on the PC
  and full-rate encode/transport.
  AppSW/`XR_FB_space_warp` remains a separate cooperative contract and is not
  enabled by this server color-only path.
- Quest 3S at 72 Hz reached real 36-to-72 synthesis for 182 candidates. The
  encode/network watchdog then returned safely to 72 Hz real frames after the
  headset reported missing shards and stale intervals.
- The ASW fallback, OpenXR runtime ownership and feedback-generation fixes are
  physically exercised. Continuous Always operation and right-eye color
  correctness are not yet accepted.
- Physical Quest 2/3S endurance and multi-vendor GPU validation are still
  required before promoting the pre-release.

## Implemented locally: capability-gated client-side SSW-like backend

The functional Vulkan `headset` backend synthesizes the missing color-only
frame after decode, near the display, and leaves final head-pose correction to
headset ATW. It keeps two real stereo history images, removes relative pose/FOV
motion before its 1/16 -> 1/4 residual-flow search, and gates the fragment warp
locally by confidence. The backend policy is:

- `auto`: detect the Vulkan client capability and timing per session, select
  `headset` when viable, otherwise remain on real frames plus ATW;
- `headset`: use the Vulkan generic compute path; the QCOM EGL/AHardwareBuffer
  Motion Engine bridge is implemented and probed but does not yet feed the
  Vulkan warp;
- `server`: use the current experimental PC-side ASW-like implementation;
- `off`: full-rate real frames and ATW only.

The QCOM capability is detected dynamically rather than inferred from a
headset model, but it is currently diagnostic/optional because its EGL/
AHardwareBuffer flow is not connected to the Vulkan warp. No private Virtual
Desktop API is available or required. The current client policy uses
pose/FOV/generation continuity, timing/deadline signals and the shader's local
confidence gate; global GPU confidence, scene-cut and disocclusion metrics are
not yet connected. Physical acceptance still requires the
[client-side SSW matrix](client-side-ssw.md).

The first client acceptance case is 36 real FPS -> 72 display Hz. It must
transmit only the real frames, produce intermediate slots locally, and
maintain a safe fallback to complete real frames plus ATW for scene cuts, low
confidence, missing capability, thermal pressure or a missed deadline. The
server 36 -> 72 result already recorded in this document is not physical
acceptance of the Vulkan headset path or evidence that the QCOM accelerator is
feeding the warp.

## Completed locally: stable FSR 1 spatial path

- Added a 32-bit Vulkan FSR 1 EASU pass followed by RCAS before WiVRn
  foveation/encoding.
- Added explicit negotiation of the headset's unscaled recommended resolution.
- Kept FSR spatial processing stateless across pause and reconnect.
- Built the server and matching Android debug APK, validated both FSR shaders
  as Vulkan 1.3 SPIR-V, verified APK v2 signing, and passed all local tests
  (`connection-lifecycle`, `scaling-watchdog` and `fsr-config`).

This backend is not AMD FidelityFX FSR 3. Spatial upscaling uses AMD's
open-source FSR 1 EASU/RCAS algorithm. The current runtime ASW backend is a
separate, opt-in optical-flow implementation and remains Off by default.

The active FSR path should produce a marker equivalent to:

```text
FSR 1 spatial upscaling enabled: EASU ... -> ... per eye, RCAS sharpness=... stops
```

## Current phase: physical revalidation and hardening

1. Instrument the ASW optical-flow handoff, per-eye encode completion and
   packet delivery to locate the 72 Hz missing-shard deadline failure. Preserve
   the current automatic fallback while doing so.
2. Revalidate the earlier right-eye cream/purple tint with continuous
   synthetic delivery and verify both eyes use the correct history, image view
   and color representation.
3. Compare Meta compositor filters, SGSR1, FSR1 and fused bilinear at matched
   render/target sizes. The current 80% to 119% Quest 3S observation is not a
   quality acceptance result.
4. Automated fault-proxy matrix: TCP-only, UDP-only, combined cuts, delay,
   handshake closure, address change, incompatible headset, and server shutdown
   from every state.
5. Visual comparison with FSR 1 disabled and enabled, including fine text,
   rotation, translation, disocclusion and transparent content.
6. Complete Quest 2 at 72/80/90 Hz, Quest 3S at 90 Hz and Intel/AMD/NVIDIA
   Vulkan and encoder coverage.
7. Physical release gate: two hours, twenty Wi-Fi cuts, twenty voluntary
   disconnects, no native crash, orphan thread, retained lock, or persistent
   MediaCodec failure.
8. Harden the implemented client-side Vulkan SSW-like path: complete `.comp`
   shader build registration, dynamic QCOM capability telemetry, and explicit
   `headset`/`server`/`auto` backend reporting without implying that QCOM feeds
   the Vulkan warp.
9. Compare client-side 36 -> 72 against full-rate real frames and server-side
   36 -> 72 for pose age, network bytes, decoder load, headset thermals,
   optical-flow deadlines and perceptual artifacts.

## Safety constraints

- Never replace a submitted frame with an incomplete generated image.
- Never use stale frames, poses, packets, or temporal history from an older
  connection generation.
- Do not describe FSR 1 spatial reconstruction or the color-only residual field
  as AMD FSR 3, real application motion, or real scene depth.
- Do not describe server-side ASW as client-side SSW, AppSW, or a reduction in
  network bandwidth; synthetic server frames still traverse encoder and
  transport.
- Do not claim a Virtual Desktop private API or a Motion Engine merely because
  a headset model is known; capability detection must be dynamic and optional.
- Do not bypass IPC version checks between unrelated installed/build-tree
  runtimes.
- Keep normal composition available for every failure.
