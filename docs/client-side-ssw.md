# Client-side SSW-like frame synthesis

Status date: 2026-08-16. Status: **experimental implementation contract**.
The capability-gated Vulkan client backend is functional in this checkout: it
keeps a bounded real-frame history, computes pose-normalized residual flow and
renders the local color-only warp. It is not the default and still needs the
physical acceptance matrix below. The current physical result reported in
this checkout is the separate server-side color-only ASW backend described in
[fsr-frame-generation.md](fsr-frame-generation.md).

This document uses **SSW-like** for a WiVRn client implementation with the
same useful placement as Virtual Desktop's Synchronous Space Warp: synthesis
after decode, close to the headset display. It does not claim that WiVRn has
access to Virtual Desktop's private interfaces, nor does it require them.
Qualcomm's Adreno Motion Engine is an optional hardware capability, not a
portable API dependency.

The Vulkan path is the functional client backend and does not require the
Motion Engine. The QCOM EGL/AHardwareBuffer bridge and probe are implemented
and covered by the available tests, but they do not currently feed the Vulkan
warp; the probe is diagnostic and a future acceleration route.

## What each warp actually does

These names describe different contracts and must not be used
interchangeably:

| Mechanism | Inputs | Where it runs | What it can correct | WiVRn status |
| --- | --- | --- | --- | --- |
| ATW/TimeWarp | A submitted image and a newer headset pose | HMD runtime/compositor | Mostly head rotation; positional correction when depth is available | Remains owned by the headset runtime for every path |
| Server ASW-like | Two PC-side color histories, poses and optical flow | WiVRn compositor, before encode | Color-only scene extrapolation plus pose reprojection | Current experimental backend; full-rate encode/transport |
| AppSW / `XR_FB_space_warp` | Application color, depth and semantically correct motion vectors | Cooperative app/runtime compositor | Scene motion, depth parallax and late pose correction | Not enabled by the current runtime ASW path |
| Client-side SSW-like | Decoded color history, render poses and a target pose | WiVRn client, after decode | Color-only scene extrapolation close to presentation | Functional Vulkan backend; experimental, capability/timing gated and not default |

ATW is the final safety net, not a scene-motion synthesizer. It can make a
late head pose look current while a moving hand, wheel, particle system or
HUD remains the old image. Client-side SSW-like synthesis does not replace
ATW; the generated image still goes through the normal headset compositor and
late pose correction.

AppSW is a cooperative application-to-compositor contract. Its depth and
motion-vector semantics are not produced automatically by
`XR_FB_space_warp`, and an optical-flow field estimated from a compressed
stream is not automatically a valid NDC motion-vector buffer. A WiVRn stream
must therefore keep the AppSW path separate from the color-only path.

## Backend policies

The backend names below are architectural policy names. The current headset
setting remains **Frame synthesis: Off / Auto / Always**. `auto` negotiates the
functional Vulkan `headset` path only when the headset advertises its color
capability; it otherwise leaves the session on real frames plus ATW. The
`server` backend remains an explicit opt-in and `headset` does not claim that a
private Virtual Desktop API is available.

| Policy/backend | Selection rule | Data path | Status and safe result |
| --- | --- | --- | --- |
| `off` | Never synthesize | Full-rate real frames + ATW | Stable baseline |
| `auto` | Probe headset Vulkan capability and timing; select `headset` when viable, otherwise remain on real frames | Capability-dependent | Adaptive policy; does not silently enable `server` |
| `headset` | Request client-side color-only synthesis | Half-rate real video, local intermediate frames, then ATW | Functional Vulkan path; experimental and deadline-gated, with optional QCOM probe not yet wired into the warp |
| `server` | Request the current PC-side color-only ASW-like backend | Half-rate application pacing but full-rate encoded/transmitted output, then ATW | Experimental; does not reduce network bandwidth |

`auto` is a policy, not a fourth optical-flow algorithm. An explicit
`headset` request must not silently turn into a server stream; when the Vulkan
path is unavailable or misses its deadline it returns to real frames plus ATW.
An explicit `server` request is subject to the server watchdog. Every fallback
is session-only and must not overwrite the saved preference.

### Dynamic Motion Engine capability

The Adreno Motion Engine, where exposed by a headset/vendor runtime, may be a
low-latency acceleration route for the `headset` backend. Its EGL/
AHardwareBuffer implementation and dynamic probe are present and tested, but
the resulting flow is not yet connected to the Vulkan warp. WiVRn must detect
this capability dynamically for each session rather than infer it from a
headset model, chipset name or product family. The current Vulkan path remains
the generic compute implementation; telemetry can report the QCOM capability
as diagnostic (`unknown`, `available` or `unavailable`) without selecting it
as the active warp implementation.

Detection is only a gate for the optional accelerator. The Vulkan backend
already applies pose/FOV continuity, timing and a local shader confidence gate;
a reported Motion Engine is not a quality or deadline guarantee. Global
per-pixel confidence, scene-cut and disocclusion GPU metrics are not yet wired
to the policy. If the generic Vulkan path misses its budget, `auto` falls back
to normal ATW; it does not silently enable `server`.

No private Virtual Desktop API is assumed or required. If a vendor later
offers a documented public acceleration interface, it belongs behind this
capability boundary and must have a generic fallback.

## 36 -> 72 example and data flow

At 72 Hz the display has one 13.89 ms slot per refresh. A 2:1 schedule lets
the PC render 36 real application frames, each with approximately 27.78 ms of
application budget, while the headset presents 72 slots:

```text
PC application:       R0 -------- R1 -------- R2 --------   (36 real FPS)
Network video:        R0 -------------------- R1 --------   (real frames only)
Headset SSW-like:       S0          S1          S2          (local intermediates)
Headset compositor:    ATW -> 72 Hz display timing and late pose correction
```

The client keeps the pose and display timestamp belonging to each real frame.
The Vulkan flow pass removes the relative head rotation and FOV mapping before
estimating residual color motion; the latest real pose is preserved for the
ordinary OpenXR layer so ATW performs the final late correction. The generated
image is never sent back over the network.

| Display refresh | Real application rate | Real-frame application budget | Synthesized slots |
| ---: | ---: | ---: | ---: |
| 72 Hz | 36 FPS | 27.78 ms | 36/s |
| 80 Hz | 40 FPS | 25.00 ms | 40/s |
| 90 Hz | 45 FPS | 22.22 ms | 45/s |
| 120 Hz | 60 FPS | 16.67 ms | 60/s |

The synthesized slot itself still has only one display period. At 120 Hz
that is 8.33 ms for synthesis, composition and the remaining runtime work;
the Qualcomm figures sometimes cited for a Motion Engine are performance
examples, not WiVRn requirements.

## Implemented Vulkan headset color-only algorithm

The minimum implementation is deliberately independent of game changes,
depth buffers and application motion vectors:

1. Decode real frame `N` and associate its `frame_id`, connection generation,
   per-eye pose, FOV, display timestamp and foveation/crop metadata.
2. Keep `N-1` and `N` in a bounded ring buffer. Clear the history after
   reconnect, stream reconfiguration or a rejected generation.
3. Keep each real frame's pose/FOV and remove relative head rotation in the
   flow shader, rather than allowing ATW to receive the same rotation twice.
4. Estimate reduced-resolution color/luma residual flow, filter it with the
   shader's confidence gate, and extrapolate `N` to the missing display time.
   The Vulkan path uses a bounded 1/16 -> 1/4 block-matching pyramid; the
   detected QCOM accelerator is not yet connected to this warp.
5. Reject the candidate when confidence, scene continuity or deadline checks
   fail. Otherwise submit it through the ordinary composition path and retain
   ATW for the final pose correction.

The client implementation is deterministic and bounded. It does not require a
neural network, unbounded history or a blocking side channel.
Depth-assisted and `XR_FB_space_warp` routes are later enhancements; they must
not change the color-only fallback contract.

## Color-only limitations

Color-only synthesis is useful because it works with unmodified PC games and
does not add a depth/motion-vector transport stream, but its quality ceiling
is lower than cooperative AppSW:

- compressed HEVC/AV1 artifacts, ringing, foveated crops and changing
  peripheral sharpness can be mistaken for motion;
- disocclusions cannot reveal pixels that were never rendered, producing
  stretched backgrounds or trails around hands and nearby objects;
- non-linear acceleration, oscillation, rotating props and fast controllers
  violate simple extrapolation assumptions;
- a flattened HUD, text, reticle or menu can warp with the 3-D scene;
- transparency, particles, reflections and animated materials do not have one
  rigid motion field;
- low-texture or dark scenes provide weak optical-flow evidence;
- the GPU/DSP, decoder and thermal budget moves to the headset, potentially
  causing throttling and a worse result than stable real frames.

The quality criterion is perceptual and temporal, not the display FPS counter.
The local shader confidence gate protects individual pixels; global confidence
and artifact counters still need to be connected before they can drive a
session-wide disable decision.

## Fallback and safety contract

| Trigger | Required action | What must remain true |
| --- | --- | --- |
| Motion Engine is `unknown`/unavailable, or no generic compute budget | `auto`/explicit `headset`: return to real frames; `server` is selected only explicitly | ATW remains active; no blocked stream |
| Low flow confidence, inconsistent forward/backward flow or disocclusion | Present the latest complete real frame with ATW | Never display an incomplete generated image |
| Scene cut, teleport or snap-turn | Do not mix the two scenes; use the new real frame or ATW for the slot | No history from the old scene is reused |
| Synthesizer misses its display deadline | Drop that candidate and present a complete real/reprojected frame; repeated misses disable the backend for the session | Saved preference is unchanged |
| Decoder loss, stale packet or wrong connection generation | Reject the packet/history; request normal decoder recovery/IDR as applicable | Old-session pixels, poses and counters cannot leak into the new session |
| Thermal pressure or sustained headset GPU budget overrun | Reduce flow complexity/resolution or disable `headset` | Decoder and compositor keep their deadline |
| Server optical-flow or encode/network watchdog trips | Disable `server` until reconnect and pace/transmit real frames | No disconnect, black frame or stale synthetic cadence |

The final fallback is normal full-rate composition plus ATW. Fallback must be
observable and session-only. A safe 1–2 frame conservative fallback is better
than extrapolating through a known discontinuity.

## Telemetry and observability

The existing runtime telemetry reports requested/effective scaling and frame
synthesis modes, real and synthetic frame counts, aggregate GPU timing,
missed-interval percentage and the fallback reason. Server logs also record
mode selection and safety transitions. For the client Vulkan path, the policy
currently uses pose/FOV and generation continuity, timing/deadline signals and
the shader's local confidence gate. Global per-pixel confidence, scene-cut and
disocclusion GPU metrics are not yet connected.

The target headset event should additionally identify:

- requested policy and effective backend (`off`, `headset`, `server`);
- Motion Engine capability (`unknown`, `available`, `unavailable`) and the
  selected implementation (currently generic Vulkan compute; a future
  Motion Engine path remains diagnostic);
- real, synthetic, reprojected and discarded frame counts per connection
  generation;
- synthesis time (P50/P95/P99), deadline misses, confidence coverage,
  disocclusion/scene-cut counts and pose age at synthesis; confidence and
  scene-cut coverage remain planned until the global GPU metrics are wired;
- video bitrate, auxiliary bytes (normally zero for color-only), decoder and
  headset GPU/thermal observations;
- the exact session-only fallback reason, including capability, low
  confidence, scene cut, thermal, optical-flow deadline and
  encode/network deadline.

The headset compact view should make it possible to distinguish “36 real / 72
display” from “72 real / 72 display”, and to tell whether a synthetic slot was
generated by the headset or the server. Telemetry must never be interpreted as
proof that the image is equivalent to a native 72/90/120 FPS render.

## Acceptance matrix

Run every backend against a full-rate real-frame control and an Off/ATW
control. A row is not accepted merely because the display counter reaches the
target refresh; it must preserve complete frames, pose timing and comfort.

| Dimension | Required cases | Evidence/pass condition |
| --- | --- | --- |
| Cadence | 36 -> 72, 40 -> 80, 45 -> 90, and 60 -> 120 where supported | Application and display rates are separately reported; no sustained missed deadlines |
| Backend policy | Off, Auto, explicit `headset`, explicit `server` | Effective backend and fallback are visible; explicit requests do not silently change backend |
| Capability | Motion Engine available, unavailable, unknown; generic compute; no compute | Dynamic probe is per session; no headset-model assumption; safe fallback in every case |
| Head/scene motion | Static world with head rotation; lateral strafe by a near wall | No persistent judder, double edge or black border; ATW remains active |
| Local motion | Hands/controllers, avatar motion, rotating props | Bounded trails/deformation; confidence fallback activates when flow is invalid |
| Content layers | HUD/text/reticles, menus, particles, transparency and reflections | Text remains readable or the backend falls back; no layer corruption is hidden by FPS claims |
| Discontinuity | Snap-turn, teleport, scene cut, reconnect and stream reconfigure | No mixed scenes; history and connection generation are reset |
| Transport/codec | HEVC/AV1, low bitrate, packet loss/jitter and missing shards | No stale synthetic cadence; server mode reports full-rate transport; client mode degrades locally |
| Sustained load | Quest 2/3/3S, other Android runtimes, 72/80/90/120 Hz as available | P95/P99 synthesis and thermal behavior remain within budget over a long session |

The current physical result is the 72 Hz `server` case: 36 Hz real
application pacing and 72 Hz encode/transport were reached, then the
encode/network watchdog returned the session to real 72 Hz after missing
shards. That validates server safety recovery, not physical acceptance of the
Vulkan headset path or integration of the QCOM accelerator. Quest 2, Quest 3S
at 90 Hz, headset-backend coverage, long sessions and multi-vendor validation
remain open.

## Related documentation

- [Quest compositor scaling and runtime frame synthesis](fsr-frame-generation.md)
  — current server backend, physical status and scaling telemetry.
- [Roadmap](roadmap.md) — implementation phases and release gates.
- [Connection lifecycle](connection-lifecycle.md) — generation filtering,
  reconnect history reset and transport recovery.
