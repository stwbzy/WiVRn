# Spatial reconstruction on PC or headset

WiVRn can reconstruct a reduced per-eye game image to an independent target
resolution. The feature processes only the current decoded frame and never
injects code into the streamed application.

The available modes are **Off**, **PC — FSR1**, **Headset — SGSR1**,
**Headset — FSR1**, and **Headset — NIS**. Off is the default. Quest/Adreno
shows all modes and recommends SGSR1; other GPUs initially expose only Off and
PC FSR1. Legacy `fsr_upscaling` configuration is deliberately migrated to Off.

## Configuration contract

- **Game resolution** is advertised to the PC OpenXR application.
- **Target resolution** is the final headset swapchain size. It is limited to
  100% normally and 150% with extended configuration, and is always clamped
  to the OpenXR runtime limit.
- **Foveated encoding** controls the non-uniform transport reduction.
- **Bitrate mode** defaults to automatic, with manual available. Automatic scales the user's budget
  by the aligned, actually encoded area relative to native resolution with
  default foveation, clamped between 5 Mbit/s and the budget.

Equal game and target sizes bypass reconstruction. A smaller target uses the
existing fused bilinear downscale; an upscaler is never used for downscaling.
An upscale greater than 2x on either axis is rejected safely. With **Off**, a
target above game resolution is ignored and the game-sized swapchain is kept.
Settings that change negotiated sizes are locked during a session.

## Pipelines

PC FSR1 performs:

```text
OpenXR layers → EASU → RCAS → foveated encoding → network → decode → swapchain
```

Headset modes perform:

```text
OpenXR layers → foveated encoding → network → decode → defoveate to game size
              → SGSR1 / FSR1 / NIS → target swapchain → WiVRn UI layer
```

The headset path uses a per-eye array image between defoveation and spatial
reconstruction. SGSR1 is a fragment pass; FSR1 uses EASU then RCAS; NIS uses
the official 32x24 NVScaler compute layout and filter banks, followed by a
color-safe copy from a private block-aligned RGBA16F image. Alpha is kept
unassociated while filtering and premultiplied again for composition, avoiding
color bleed at passthrough edges. If an image, shader, or algorithm pipeline
cannot be created, the complete target is filled through the bilinear fallback
and a visible toast plus log entry is emitted. Set
`WIVRN_SPATIAL_FORCE_FALLBACK=1` in a development build to exercise it.

SGSR1 is adapted from Qualcomm's BSD-3-Clause Vulkan GLSL shader, FSR1 from
AMD's MIT-licensed FidelityFX FSR1 EASU/RCAS formulation, and NIS is adapted
from the MIT-licensed NVIDIA Image Scaling SDK 1.0.3 NVScaler source and
coefficient tables. Source headers and binary notices retain their licenses.

CMAA2 is not offered as a reconstruction mode: it is a conservative
morphological anti-aliasing post-process, not an image upscaler. Its compute
implementation can be ported to Vulkan/Linux, but it would complement one of
the modes above rather than reduce the transported resolution by itself.

## Verification matrix

Build APK and server from the same commit; the protocol hash intentionally
changes with this negotiation. With Application SpaceWarp disabled at 90 Hz,
test 100/100 Off, 80/80 Off, 80/100 PC FSR1, each 80/100 headset algorithm,
and 65/100 SGSR1. Compare all modes first at the same manual bitrate, then in
automatic mode.

Use high-contrast text, diagonals, particles, avatars, rapid head movement,
and both-eye boundary patterns. Record encode/network/decode time, the separate
defoveation and upscale GPU plots, temperature, battery, bytes, and lost frames.
Run each mode for 30 minutes and finish with a two-hour SGSR1 soak.

Acceptance requires no black frames, flashes, cross-eye contamination, or
Vulkan errors; stable 1:1 cadence; headset upscale below 2 ms p95; and at least
1 ms before the 11.111 ms deadline. Automatic mode must reduce measured traffic
when encoded area drops. NIS is not accepted merely because its pipeline
initializes: its larger compute workload must meet the same thermal and p95
limits on the target Quest model.

FSR2/3/4 temporal reconstruction is intentionally absent: arbitrary OpenXR
applications do not universally provide the runtime with motion vectors,
depth, jitter, and object data required by those algorithms.
