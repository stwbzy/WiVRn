# FSR 1 spatial upscaling with experimental frame generation

Status date: 2026-08-10.

This branch contains two independent Vulkan compositor features that can be
enabled together: AMD FidelityFX FSR 1 spatial reconstruction (EASU followed
by RCAS), and the frame-generation path tested in the earlier WiVRn `v26.6.2`
experiment. Both run inside the current pause/resume lifecycle.

The spatial shader is based on AMD's MIT-licensed
[FidelityFX-FSR source](https://github.com/GPUOpen-Effects/FidelityFX-FSR),
including its documented EASU 1x–4x output-area range and the required
EASU-then-RCAS pass order.

> [!IMPORTANT]
> This is **not AMD FidelityFX FSR 3**. The spatial path is a 32-bit GLSL port
> of AMD's open-source FSR 1 EASU/RCAS algorithm. Frame generation is a
> separate WiVRn implementation using stereo color history, rotational pose
> reprojection, and confidence-gated residual optical flow; it does not infer
> real scene depth or modify VRChat/game binaries.

## Current status

Implemented in the current tree:

- negotiation of both reduced render size and the headset runtime's original
  recommended per-eye size;
- FSR 1 EASU reconstruction from the reduced render to that recommended size;
- a separate RCAS pass after EASU, with configurable sharpness;
- preservation of WiVRn's foveation ratio at the reconstructed resolution;
- identical EASU/RCAS processing for real and generated frames;
- persistent two-eye color history at the final render extent;
- a metadata-backed candidate path for applications that submit motion and
  depth through `XR_EXT_frame_synthesis`;
- an explicitly enabled color-only fallback for applications such as VRChat;
- midpoint pose compensation plus a quarter-resolution residual motion field;
- candidate handoff through the existing foveation and encoder pipeline;
- 45-to-90 double-rate transport with even generated IDs and odd application
  IDs;
- Android MediaCodec timestamp correlation using the advertised stream rate;
- reset of history, flow, poses and pacing on pause/reconnect;
- normal-composition fallback when a candidate cannot be produced.

The current port builds locally as both a PC server and Android debug APK. The
EASU and RCAS SPIR-V modules pass `spirv-val` for Vulkan 1.3, the APK verifies
with Android signature scheme v2, and the `connection-lifecycle` and
`fsr-config` tests pass (2/2). The earlier `v26.6.2` frame generator was
visually exercised with VRChat and Quest at 90 Hz; the new combined FSR 1 path
still needs a fresh physical visual/stability run before it is considered
revalidated.

## Environment variables

| Variable | Default | Purpose |
| --- | --- | --- |
| `WIVRN_FSR_UPSCALING` | `0` | Enable FSR 1 EASU + RCAS when the headset render resolution is below 100%. |
| `WIVRN_FSR_SHARPNESS` | `0.2` | RCAS sharpness in stops: `0` is strongest; accepted range is `0`–`2`. |
| `WIVRN_FSR_FRAME_GENERATION` | `0` | Master switch: allocate history and candidate resources. |
| `WIVRN_FSR_FRAME_GENERATION_COLOR_ONLY` | `0` | Permit color-history generation when the application supplies no motion/depth metadata. |
| `WIVRN_FSR_FRAME_GENERATION_HANDOFF` | `0` | Send a complete candidate through foveation and the encoder. Double-rate mode enables this implicitly. |
| `WIVRN_FSR_FRAME_GENERATION_DOUBLE_RATE` | `0` | Pace the application at half rate and send candidate/application pairs at the headset rate. |
| `WIVRN_FSR_FRAME_GENERATION_SEARCH_RADIUS` | `2` | Residual optical-flow radius, clamped from 1 through 4. |

Leave the search radius unset for initial testing. A larger value costs more
GPU work and cannot correct missing depth, disocclusion, or pose errors.

FSR 1 needs a lower-resolution input. In the headset settings, disconnect and
set **Render resolution** to 70–85% before connecting. At 100%, the server logs
that spatial upscaling is inactive rather than wasting GPU time on a 1:1 EASU
pass. The implementation rejects scales below FSR 1's documented 4x output
area limit; 50% per axis is the lower boundary.

## Build

From this checkout:

```bash
cd /home/stwby/Proyectos/WiVRn-fsr

cmake --preset server-no-system-boost \
  -DWIVRN_BUILD_TEST=ON \
  -DEigen3_DIR=/home/stwby/Proyectos/deps/eigen-install/share/eigen3/cmake \
  -DCMAKE_PREFIX_PATH=/home/stwby/Proyectos/deps/local

cmake --build build-server-no-system-boost -j2
ctest --test-dir build-server-no-system-boost --output-on-failure

ANDROID_HOME=/home/stwby/Android ./gradlew assembleDebug
```

Matching local artifacts:

```text
build-server-no-system-boost/server/wivrn-server
build-server-no-system-boost/server/wivrnctl
build-server-no-system-boost/openxr_wivrn-dev.json
build-server-no-system-boost/tools/wivrn-fsr-openxr-harness
build/outputs/apk/debug/WiVRn-fsr-debug.apk
```

The PC server/runtime and headset APK must come from this same source tree.
The client changes are required both for double-rate MediaCodec timestamps and
for sending the unscaled headset-recommended resolution to the server.

Install that matching APK without clearing the pairing/resume token:

```bash
adb install -r build/outputs/apk/debug/WiVRn-fsr-debug.apk
```

## Run FSR 1 only

This is the simplest visual comparison and should be tested before adding
frame generation. Set headset **Render resolution** to 70–85%, reconnect, and
run:

```bash
XRT_PRINT_OPTIONS=1 XRT_LOG=info \
WIVRN_FSR_UPSCALING=1 \
WIVRN_FSR_SHARPNESS=0.2 \
build-server-no-system-boost/server/wivrn-server \
  --no-fork --early-active-runtime
```

This runs EASU and RCAS for each real application frame while leaving the
application and stream cadence unchanged. Compare fine text and diagonal
edges against a control run with both variables unset. `0` is the strongest
RCAS setting; increase the stops toward `2` if edges shimmer or look
oversharpened.

## Run FSR 1 and 45-to-90 frame generation together

First set the headset **Render resolution** below 100% as described above and
disable headset **Application SpaceWarp**. The server double-rate mode already
halves application pacing; leaving the headset option active applies a second
divider. Then stop any installed/user-service server so only one process owns
port 9757 and run:

```bash
XRT_PRINT_OPTIONS=1 XRT_LOG=info \
WIVRN_FSR_UPSCALING=1 \
WIVRN_FSR_SHARPNESS=0.2 \
WIVRN_FSR_FRAME_GENERATION=1 \
WIVRN_FSR_FRAME_GENERATION_COLOR_ONLY=1 \
WIVRN_FSR_FRAME_GENERATION_HANDOFF=1 \
WIVRN_FSR_FRAME_GENERATION_DOUBLE_RATE=1 \
build-server-no-system-boost/server/wivrn-server \
  --no-fork --early-active-runtime
```

The processing order is deterministic:

```text
OpenXR layers at reduced render size
  -> SpaceWarp candidate at reduced render size
  -> FSR 1 EASU
  -> FSR 1 RCAS
  -> WiVRn foveation / YCbCr
  -> encoder
```

In double-rate mode, the generated frame goes through the complete EASU/RCAS
chain first, then the real frame goes through the same chain. Separate Vulkan
descriptor banks prevent the second dispatch from changing the first one's
source.

Expected additional marker:

```text
FSR 1 spatial upscaling enabled: EASU 1465x1535 -> 1832x1919 per eye, RCAS sharpness=0.20 stops
```

Exact sizes depend on the headset and selected resolution. If the log says
`requested but inactive`, lower **Render resolution**, reconnect, and verify
that the matching APK is installed.

Expected cadence is application 45 Hz / stream 90 Hz at a 90 Hz headset rate,
or application 60 Hz / stream 120 Hz at 120 Hz. Application 30 Hz / stream
60 Hz while the headset is at 120 Hz means headset **Application SpaceWarp**
is still enabled.

## Run the 45-to-90 color-only path

Stop any installed/user-service server first so only one process owns port
9757. Then run the old four-variable command from the project root:

```bash
XRT_PRINT_OPTIONS=1 XRT_LOG=info \
WIVRN_FSR_FRAME_GENERATION=1 \
WIVRN_FSR_FRAME_GENERATION_COLOR_ONLY=1 \
WIVRN_FSR_FRAME_GENERATION_HANDOFF=1 \
WIVRN_FSR_FRAME_GENERATION_DOUBLE_RATE=1 \
build-server-no-system-boost/server/wivrn-server \
  --no-fork --early-active-runtime
```

At a 90 Hz headset rate, the application is paced at 45 Hz and WiVRn sends an
interval candidate followed by the real application frame at 90 Hz. It must
not advertise a 180 Hz video stream.

Expected server markers:

```text
FSR frame-generation option: env=1 parsed=1
FSR color-only optical-flow search radius: 2
FSR frame-generation double-rate transport enabled: application rate 45Hz, stream rate 90Hz
Video stream description: ... frame_rate=90.0Hz refresh_rate=90.0Hz
FSR frame-generation candidate dispatched: mode=pose-compensated-color-only ...
FSR frame-generation candidate handed off to foveation and encoder; diagnostic double-rate mode
FSR double-rate pair submitted: candidate_frame=0 application_frame=1 ...
```

Expected headset marker:

```text
MediaCodec stream 0 cadence: frame_rate=90Hz refresh_rate=90Hz
```

The exact image size depends on headset and stream configuration. A
`180Hz/90Hz` rate pair means an older or mismatched server is running.

## Other useful modes

Recommended isolation matrix:

| Test | Spatial FSR | Frame generation | Purpose |
| --- | --- | --- | --- |
| Control | Off | Off | Verify the unchanged WiVRn path. |
| FSR only | On | Off | Inspect reconstruction, text and RCAS sharpness. |
| Generation only | Off | On | Inspect cadence and SpaceWarp artifacts independently. |
| Combined | On | On | Confirm identical EASU/RCAS treatment of real and generated frames. |

Allocate and inspect inputs while keeping normal composition:

```bash
WIVRN_FSR_FRAME_GENERATION=1 \
  build-server-no-system-boost/server/wivrn-server
```

Same-rate diagnostic candidate handoff:

```bash
WIVRN_FSR_FRAME_GENERATION=1 \
WIVRN_FSR_FRAME_GENERATION_COLOR_ONLY=1 \
WIVRN_FSR_FRAME_GENERATION_HANDOFF=1 \
  build-server-no-system-boost/server/wivrn-server
```

Double-rate generation from application-provided motion/depth metadata does
not need the color-only switch:

```bash
WIVRN_FSR_FRAME_GENERATION=1 \
WIVRN_FSR_FRAME_GENERATION_DOUBLE_RATE=1 \
  build-server-no-system-boost/server/wivrn-server
```

For VRChat through OpenComposite, use the complete four-variable command:
that path normally exposes projection color but not application motion vectors
or depth.

## Diagnostic harness

The standalone harness can submit normal projection, EXT frame-synthesis
metadata, or FB Space Warp metadata. With the matching build-tree server and a
connected headset, run from a second terminal:

```bash
XR_RUNTIME_JSON="$PWD/build-server-no-system-boost/openxr_wivrn-dev.json" \
DISABLE_XR_APILAYER_NOV3_xess_upscaler=1 \
XR_LOADER_DISABLE_IMPLICIT_LAYERS=1 \
build-server-no-system-boost/tools/wivrn-fsr-openxr-harness \
  --normal --visual-pattern --frames 120 --timeout-ms 20000
```

Use `--normal` to exercise the same color-only condition as VRChat. Use
`--frame-synthesis` or `--space-warp` to validate the metadata transport path.
The harness is a diagnostic application, not a substitute for a real-game
visual test.

Do not use `IPC_IGNORE_VERSION=1` to mix an installed runtime with unrelated
build-tree artifacts. Protocol and shared-memory layouts must match.

## Reconnection behavior

On pause or link loss, the compositor clears:

- pending encode requests;
- color history and generated output validity;
- optical-flow validity;
- previous poses and display time;
- pacer feedback samples.

On resume, the encoder is reset so the stream starts with fresh parameter sets
and an IDR. Frame IDs remain correlated with the OpenXR pacer, preventing
post-reconnect feedback from being assigned to a pre-reconnect frame.
EASU/RCAS has no temporal history to reuse across generations; its persistent
images retain only Vulkan layout state and are overwritten before each use.

## Safety and limitations

- Unset `WIVRN_FSR_FRAME_GENERATION` to use the unchanged normal path.
- Unset `WIVRN_FSR_UPSCALING` to bypass EASU/RCAS completely.
- An incomplete candidate is never handed to the encoder.
- If no valid candidate exists, the generated slot uses a complete normal
  composition frame and keeps the transport cadence contiguous.
- The color-only path corrects rotational pose and estimates residual 2D
  motion. Positional movement, disocclusion, animated objects and transparency
  can still show artifacts because there is no real scene depth.
- OpenXR metadata transport does not prove that a game supplies useful motion
  vectors or depth.
- FSR 1 is spatial and does not reconstruct temporal detail. It also does not
  turn the custom color-only generator into AMD FSR 3 Frame Generation.

## Physical revalidation checklist

1. Install the matching debug APK with `adb install -r`.
2. Disable headset **Application SpaceWarp** and confirm the server and Quest
   both report `90/90 Hz` at 90 Hz, or `120/120 Hz` at 120 Hz.
3. Confirm the server reports an EASU input smaller than its output and the
   video stream uses the reconstructed/foveated dimensions.
4. Confirm decoded frame IDs are contiguous and candidates dispatch after the
   first color-history capture.
5. Check fast head rotation, translation, hands/avatars, fine text and transparent
   surfaces for artifacts.
6. Test a disconnect/reconnect and confirm history starts fresh with a new
   IDR.
7. Run a normal-path control session with every `WIVRN_FSR_*` variable unset.
8. Complete a long-duration session before making a stability claim for this
   port.
