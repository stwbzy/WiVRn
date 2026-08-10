# FSR Frame Generation and connection-stability roadmap

Status date: 2026-08-09.

The fork keeps VRChat, OpenComposite, and game binaries unmodified. The
runtime/compositor owns experimental frame generation. Connection lifecycle
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

The current probe still reports:

```text
backend=none fallback=normal-composition
```

This means transport is working but visual 45→90 interpolation is not yet
implemented or validated.

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

## Current phase: real Vulkan/FSR3 backend

1. Consume the validated depth and motion-vector images at the PC compositor
   boundary.
2. Define image layouts, queue ownership, synchronization, and lifetime for
   persistent FSR3 resources.
3. Initialize a real FSR3 Vulkan backend and process two valid source frames.
4. Integrate generated-frame timestamps, pacing, and compositor/encoder
   handoff without reusing pre-reconnect history.
5. Submit generated frames only when every stereo input and synchronization
   condition is valid; otherwise retain normal composition.
6. Add metrics for backend initialization, generated frames, fallback causes,
   and pacing misses.

Frame Generation must not be presented as active until logs contain equivalent
evidence to:

```text
FSR3 backend initialized: Vulkan
FSR3 inputs valid: views=2
FSR3 generated frame submitted
```

## Following phases

1. Automated fault-proxy matrix: TCP-only, UDP-only, combined cuts, delay,
   handshake closure, address change, incompatible headset, and server shutdown
   from every state.
2. Visual and cadence validation with frame generation disabled and with real
   45→90 output enabled.
3. VRChat smoke tests without game-specific hooks or binary changes.
4. Physical release gate: two hours, twenty Wi-Fi cuts, twenty voluntary
   disconnects, no native crash, orphan thread, retained lock, or persistent
   MediaCodec failure.
5. Revisit the headset protocol only if generated-frame presentation timing
   requires information not already available on the PC.

## Safety constraints

- Never replace a submitted frame with an incomplete generated image.
- Never use stale frames, poses, packets, or temporal history from an older
  connection generation.
- Do not synthesize production depth or motion vectors from color alone.
- Do not bypass IPC version checks between unrelated installed/build-tree
  runtimes.
- Keep the normal EASU/RCAS or composition path available for every failure.
