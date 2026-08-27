# Quest compositor scaling and runtime frame synthesis

Status date: 2026-08-13. Status: **pre-release**. Meta compositor scaling may
be promoted after the physical-device matrix below passes. Server frame
synthesis remains experimental until long Quest 2 and Quest 3S sessions have
been completed.

## Scaling paths

The headset sends two independent sizes to the server:

- **Render resolution** controls the game render and the streamed source.
- **Target resolution** controls the OpenXR projection texture submitted on
  Quest. Horizon OS performs the final conversion from that layer to the panel.

The Video page provides these modes:

| Mode | Processing location | Status |
| --- | --- | --- |
| Native | Existing 1:1 client path | Stable |
| Meta compositor | OpenXR layer plus Meta compositor filtering | Recommended on Quest 1/2 |
| SGSR1 mobile | Quest fragment pass, fused with defoveation | Experimental |
| FSR1 | Linux server EASU + RCAS | Experimental |
| NIS | Preserved config value; safely falls back until a portable backend is available | Experimental |
| Bilinear fused | Quest sampling, fused with defoveation | Safe fallback |

`Meta compositor` is deliberately not called “Meta SGSR”. WiVRn requests
compositor filtering, supersampling or sharpening; OpenXR does not promise a
particular internal reconstruction algorithm. When
`XR_META_automatic_layer_filter` is available, the runtime selects the filter.
Otherwise WiVRn uses the normal sharpening/supersampling flags from
`XR_FB_composition_layer_settings`. See the OpenXR
[automatic-filter extension](https://registry.khronos.org/OpenXR/specs/1.1/man/html/XR_META_automatic_layer_filter.html)
and [layer-setting flags](https://registry.khronos.org/OpenXR/specs/1.1/man/html/XrCompositionLayerSettingsFlagBitsFB.html).

Quest 1 and Quest 2 show Meta compositor as the recommended choice. Quest Pro,
Quest 3 and Quest 3S expose Meta compositor and SGSR1 without enabling either
automatically. Unknown devices retain the conservative Native default.

An expensive reconstruction is skipped when the target is less than 10%
larger per dimension. For example, 100% -> 101% never dispatches SGSR1 or
FSR1; it uses Meta compositor when supported, otherwise fused bilinear.
Requested mode, effective mode, source/target sizes and fallback reason are
logged independently. A fallback never overwrites the saved preference.

## SGSR1 mobile

The client shader is based on Qualcomm's official SGSR1 mobile shader without
edge-direction estimation. It keeps the official 12-tap adaptive
Lanczos-like reconstruction and sharpening structure, uses relaxed precision
on mobile GPUs, and retains normal FP32 behavior where relaxed precision is
not beneficial.

Upstream reference: Qualcomm's
[Snapdragon Game Super Resolution repository](https://github.com/SnapdragonGameStudios/snapdragon-gsr/tree/main/sgsr/v1).

Decoded color is filtered in its UNORM/gamma representation. Explicit sRGB
conversion, where required by the headset, happens once after reconstruction
instead of executing `pow()` for every sample. Qualcomm's BSD-3-Clause notice
is shipped as `LICENSE-SGSR1-BSD-3-Clause`.

When SGSR1 is disabled, target scaling and defoveation use the same graphics
pass and write directly to the OpenXR swapchain. There is no intermediate
upscaled image or second client pass.

## Scaling watchdog

The client evaluates 180-frame windows:

- fall back when client-pass p95 exceeds 55% of the display period;
- fall back after more than 1% missed intervals in three consecutive windows.

The session-only order is SGSR1/FSR1/NIS -> Meta compositor -> fused bilinear.
The watchdog resets on reconnect and does not change the saved selection.

## Warp terminology and backend policy

Frame synthesis is not one OpenXR feature. **ATW/TimeWarp** is the headset
runtime's late pose reprojection and remains active for both real and
generated images. **AppSW / `XR_FB_space_warp`** is a cooperative application
contract that consumes application depth and motion vectors; WiVRn does not
obtain those buffers merely by enabling an extension. The current
WiVRn-specific implementation is a separate color-only ASW-like path.

The client-side SSW-like implementation is documented in
[client-side-ssw.md](client-side-ssw.md). Its functional Vulkan backend places
color-only synthesis after decode, close to the display, and is not an
assertion that a private Virtual Desktop interface is available. The QCOM
EGL/AHardwareBuffer Motion Engine path is implemented and probed, but currently
does not feed the Vulkan warp; it remains an optional diagnostic/acceleration
route. The Vulkan backend does not require it.

The current public setting is still **Frame synthesis: Off / Auto / Always**.
For architecture and telemetry, use these backend policy names:

| Policy/backend | Meaning | Status |
| --- | --- | --- |
| `off` | Full-rate real frames plus headset ATW | Stable baseline |
| `auto` | Probe headset Vulkan capability and timing; select `headset` when viable, otherwise remain on real frames | Adaptive policy; does not silently enable `server` |
| `headset` | Client-side color-only SSW-like synthesis after decode | Functional Vulkan backend; experimental/capability-gated and not default |
| `server` | Current PC-side color-only ASW-like synthesis before encode | Experimental; full-rate encode/transport |

These names do not add undocumented controls to the current UI. `Auto` can
negotiate the functional Vulkan `headset` backend when the headset advertises
the required color capability; otherwise it remains on real frames plus ATW.
The optional QCOM probe is diagnostic and is not the source of the Vulkan warp.
`server` remains an explicit backend choice and no private vendor Motion
Engine API is assumed.

### The 36 -> 72 schedule

At 72 Hz, a 2:1 schedule renders 36 real application frames and fills the 36
intermediate display slots. The real application gets approximately 27.78 ms
per frame while each display slot still has only 13.89 ms. The functional
client Vulkan backend transmits 36 real images and synthesizes the other 36
after decode; the current server backend transmits 72 images because its
synthetic frames go through the encoder and network.

| Display | Real application | Real-frame budget | Generated slots |
| ---: | ---: | ---: | ---: |
| 72 Hz | 36 FPS | 27.78 ms | 36/s |
| 80 Hz | 40 FPS | 25.00 ms | 40/s |
| 90 Hz | 45 FPS | 22.22 ms | 45/s |
| 120 Hz | 60 FPS | 16.67 ms | 60/s |

The headset backend must keep each real frame's pose, FOV, display timestamp
and connection generation. It must remove head motion before estimating
scene flow, use a confidence gate, and leave time for ATW. Missing depth or
application motion vectors must not block the stream.

## Server ASW-like runtime frame synthesis (current experimental backend)

The old `fps_divider` control has been replaced by **Frame synthesis: Off /
Auto / Always**. A legacy JSON value of `fps_divider=2` migrates to Off and
shows a warning; it is never silently re-enabled.

Frame synthesis runs on the Linux PC:

1. retain the previous and current real composed frames;
2. estimate portable two-level Vulkan optical flow (1/16 coarse, 1/4 refine);
3. combine pose reprojection and confidence-gated residual flow;
4. extrapolate the following display interval;
5. encode and transmit real and interval frames at headset refresh rate while
   the game is paced at half rate.

Low-confidence pixels and invalid reprojections fall back to the latest real
frame, allowing headset ATW to remain effective. No additional application
frame is buffered. Synthetic frames still pass through the encoder and
network, so this reduces game rendering work but **does not reduce bandwidth**.

Auto mode enters half-rate pacing only after three sustained over-budget
windows and exits after three windows with 30% headroom. If optical flow or
encode/network feedback misses its deadline in three consecutive windows,
frame synthesis is disabled until reconnect. Off remains the default.

WiVRn does not enable `XR_FB_space_warp` or application frame-synthesis
metadata. VRChat is not modified and does not need to provide depth or motion
vectors. Quest ATW remains controlled by Horizon OS.

This is the `server` backend, not client-side SSW. It uses clean PC-side color
before compression, but its pose ages during encode/transport/decode and its
synthetic images consume full-rate network and decoder capacity. It remains an
experimental comparison/fallback backend alongside the functional,
capability-gated Vulkan headset implementation.

### Physical status on Quest 3S

The first controlled end-to-end run at 72 Hz confirmed that this is no longer
a metadata-only probe. VRChat submitted one stereo projection layer, the
server retained history and dispatched both optical-flow levels for two
1465x1535 views. In Always mode the logged cadence was 36 Hz application and
72 Hz encode/transport, and 182 backend candidates were dispatched.

That run did not pass the deadline gate. The client reported missing shards
and stale frames while the full-rate stream was active. After three failed
encode/network feedback windows, WiVRn disabled synthesis until reconnect and
restored 72 Hz application pacing. Streaming, tracking and audio remained
connected, the server reported zero discarded-generation packets, and headset
statistics returned to 72/72 without sustained stale frames.

This result validates the fallback mechanism but not Always mode. The tested
combination was 10-bit video, 50 Mbit/s and an aggressive 80% render to 119%
target configuration. Do not generalize the failure to every encoder or
network, and do not weaken the watchdog to hide it. Identify whether the
missed deadline originates in optical-flow handoff, full-rate encode or packet
delivery.

An earlier test build showed a slowly alternating cream/purple tint in the
right eye in Always mode. The controlled run fell back before a sustained
visual inspection, so stereo color correctness is still unverified.

With synthesis Off and matching feedback-generation tags, the same headset
measured 50-60 ms in WayVR and 69-83 ms in VRChat, usually about 71 ms, with
normal head motion. This is the current physical control baseline.

## Telemetry

The in-headset compact statistics show defoveation, effective local scaling,
Meta compositor filter, real/synthetic frame counts, aggregate GPU timing,
missed intervals and runtime fallback reason. The server also logs every mode
selection and safety transition.

For a client-side backend, the same telemetry distinguishes the requested
policy from the effective backend (`off`, `headset` or `server`), reports the
per-session Motion Engine capability and identifies generic Vulkan compute.
The current policy uses pose/FOV and generation continuity, timing/deadline
signals and the shader's local confidence gate. Global per-pixel confidence,
scene-cut and disocclusion GPU metrics are not yet connected; synthesis-only
P50/P95/P99 and confidence coverage remain acceptance targets rather than
current global measurements. The compact headset view should make “36 real /
72 display” distinguishable from “72 real / 72 display”.

## Build and automated checks

```bash
cmake --preset server-no-system-boost -DWIVRN_BUILD_TEST=ON
cmake --build build-server-no-system-boost -j2
ctest --test-dir build-server-no-system-boost --output-on-failure

ANDROID_HOME=/path/to/Android ./gradlew assembleDebug
```

The build compiles the portable Vulkan path and configured shaders. Unit
coverage includes the 10% selection threshold, both watchdog triggers and the
QCOM estimator capability/fallback contract. Full physical Vulkan client
coverage, including the QCOM-to-warp integration that is not yet connected,
remains an acceptance item.

The 2026-08-13 build passed all three configured tests:
`connection-lifecycle`, `scaling-watchdog` and `fsr-config`.

## Physical acceptance matrix

Test Quest 2 at 72, 80 and 90 Hz and Quest 3S at 90 Hz. At minimum cover:

- 100/101%, 100/100%, 80/80% and 80/100% render/target pairs;
- Meta normal/quality or automatic filtering, SGSR1 and fused bilinear;
- Frame synthesis Off, Auto and Always;
- backend policy cases `auto`, `headset` and `server` where the corresponding
  capability is available, including the 36 -> 72 schedule;
- Motion Engine advertised, unavailable and unknown; generic compute and no
  compute; detection must be per session rather than inferred from a headset
  model;
- head, hand and avatar motion, particles, menus and scene cuts;
- packet loss, reconnect and automatic safety disable;
- Intel, AMD and NVIDIA Vulkan drivers.

Quest 2 acceptance requires no SGSR dispatch at 100/101%, no sustained Meta
compositor judder, fallback without a disconnect or black screen, and a
two-hour VRChat session without memory growth. Quest 3/3S must show no quality
or performance regression against Native. Frame synthesis must not add more
than one display period of latency.

Current matrix coverage is limited to Quest 3S at 72 Hz. The Quest 2 matrix,
Quest 3S at 90 Hz, long sessions and Intel/AMD/NVIDIA cross-validation remain
open. Meta compositor worked on Quest 3S but looked more pixelated than SGSR1
at 80% render to 119% target; repeat that comparison at matched sizes and with
each available compositor filter before drawing a quality conclusion.

The complete client-side SSW-like matrix, including color-only limitations,
fallback rules and scene/codec/thermal cases, is maintained in
[client-side-ssw.md](client-side-ssw.md). The current physical 36 -> 72 result
belongs to the `server` backend: it validates watchdog recovery after missing
shards, not physical acceptance of the Vulkan headset path or integration of
the QCOM accelerator.
