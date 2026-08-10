# Experimental FSR Frame Generation fork

Status date: 2026-08-09.

This branch explores runtime-owned OpenXR Space Warp and AMD FSR3 Frame
Generation in WiVRn. It does not modify VRChat, OpenComposite, or game
binaries. The PC runtime/compositor is the integration point; headset changes
are permitted only when the streaming or recovery protocol requires them.

## Current result

The structural metadata path is implemented and physically exercised. Visual
Frame Generation is not implemented.

The build-tree runtime can advertise and validate:

- `XR_KHR_vulkan_enable2`;
- `XR_EXT_frame_synthesis`;
- `XR_FB_space_warp`.

It transports stereo color, depth, and motion-vector swapchains through XRT
and IPC. The compositor probe observes the metadata but deliberately retains
normal composition:

```text
FSR frame-generation compositor probe enabled: metadata-only, normal composition retained
FSR frame-generation compositor probe: ... backend=none fallback=normal-composition
```

Therefore:

- extension advertisement is not evidence of FSR3 output;
- `inputs_ready=1` proves structural input delivery, not generated pixels;
- current tests do not demonstrate visual 45→90 interpolation;
- normal composition remains the required fallback.

## Revisions and patch stack

The WiVRn base revision is:

```text
aa3a94e76d3126eedcda0719d36722b3c350b90b
```

The generated Monado history is:

```text
285a5a539 multi: transport frame-synthesis swapchains
486881b5e oxr: validate and transport frame synthesis metadata
7a357e2ae xrt: transport frame-synthesis metadata through IPC
b6799ec08 ipc: join client threads before teardown
```

The corresponding source patches are:

```text
patches/monado/0010-frame-synthesis-metadata-transport.patch
patches/monado/0011-oxr-validate-frame-synthesis.patch
patches/monado/0012-multi-frame-synthesis-transport.patch
```

Patch `0012` forwards the new swapchain metadata through the GL, Vulkan, and
multi-client compositor wrappers. The complete series applies cleanly from
the recorded base, and the generated Monado worktree is expected to remain
clean.

## Build

Configure a development server and tests without replacing the installed
runtime:

```bash
cmake --preset server-no-system-boost \
  -DWIVRN_BUILD_TEST=ON \
  -DEigen3_DIR=/home/stwby/Proyectos/deps/eigen-install/share/eigen3/cmake \
  -DCMAKE_PREFIX_PATH=/home/stwby/Proyectos/deps/local

cmake --build build-server-no-system-boost -j2
```

Important outputs:

```text
build-server-no-system-boost/server/wivrn-server
build-server-no-system-boost/server/wivrnctl
build-server-no-system-boost/openxr_wivrn-dev.json
build-server-no-system-boost/_deps/monado-build/src/xrt/targets/openxr/libopenxr_wivrn.so
build-server-no-system-boost/tools/wivrn-fsr-openxr-harness
```

Start the matching build-tree server with the diagnostic probe:

```bash
WIVRN_FSR_FRAME_GENERATION=1 \
  build-server-no-system-boost/server/wivrn-server
```

The environment variable enables observation only. It does not initialize an
FSR3 backend or alter the submitted frame.

## Diagnostic harness

`wivrn-fsr-openxr-harness` is a standalone Vulkan/OpenXR client with three
modes:

- `--normal`: stereo projection without synthesis metadata;
- `--frame-synthesis`: `XR_EXT_frame_synthesis` metadata;
- `--space-warp`: `XR_FB_space_warp` compatibility metadata.

For the metadata modes it creates two color, two motion-vector, and two depth
swapchains, locates both views, and submits one projection layer per frame.
It deliberately does not write meaningful image pixels or synthesize a frame.

Example short run against matching build-tree artifacts:

```bash
IPC_IGNORE_VERSION=1 \
  build-server-no-system-boost/tools/wivrn-fsr-openxr-harness \
  --frame-synthesis --frames 300 --timeout-ms 30000
```

Use `IPC_IGNORE_VERSION=1` only when the harness, runtime, and server were
built from this same tree. It is not permission to mix the build-tree runtime
with an installed WiVRn service. An incompatible shared-memory layout was
previously rejected by the normal version check and produced `SIGBUS` when
that check was bypassed.

Expected structural probe fields include:

```text
synthesis_layers=1
synthesis_depth_views=2
motion_vector_views=2
inputs_ready=1
backend=none
fallback=normal-composition
```

## Validation completed

The following evidence was collected with the matching build-tree server and
a Quest 3S:

- server, runtime, and harness compile successfully;
- a 300-frame physical frame-synthesis smoke test exited successfully;
- a 20,000-frame structural run exited successfully across a voluntary
  disconnect/resume;
- a physical Wi-Fi cut retained the same PC OpenXR application and restored
  synthesis metadata after reconnection;
- the Meta `xrWaitFrame` recovery fallback recreated the local XR activity and
  resumed at connection generation 3;
- the frame-generation probe continued to report normal-composition fallback,
  with no claim of generated pixels.

The lifecycle result is documented separately in
[connection-lifecycle.md](connection-lifecycle.md).

## Architecture

The work is intentionally split into distinct layers:

1. OpenXR extensions describe application-provided depth and motion-vector
   metadata and its relationship to stereo color frames.
2. XRT/IPC transports validated swapchains and per-frame metadata to the
   server compositor.
3. The compositor must own persistent Vulkan resources, image transitions,
   synchronization, and queue ownership.
4. A real FSR3 backend must process two valid source frames and their motion
   information.
5. Frame pacing must insert generated output without corrupting OpenXR timing,
   encoder cadence, or reconnection generations.

The current implementation completes layers 1 and 2 and provides a diagnostic
boundary for layer 3. It does not complete layers 3–5.

VRChat currently does not provide the required Space Warp depth and motion
vectors through this path. Correct production inputs cannot be inferred from a
color frame alone. This remains an application/runtime capability issue, not a
reason to add a game-specific hook.

Relevant specifications:

- [XR_FB_space_warp](https://registry.khronos.org/OpenXR/specs/1.0/man/html/XrCompositionLayerSpaceWarpInfoFB.html)
- [XR_EXT_frame_synthesis](https://registry.khronos.org/OpenXR/specs/1.1/man/html/XrFrameSynthesisInfoEXT.html)
- [AMD FSR3 integration guide](https://gpuopen.com/manuals/fidelityfx_sdk/techniques/super-resolution-interpolation/)
- [AMD FSR3 overview](https://gpuopen.com/fidelityfx-super-resolution-3/)

## Next implementation milestones

1. Consume the validated images at the compositor boundary.
2. Add persistent Vulkan resources and explicit synchronization.
3. Initialize and execute the FSR3 backend.
4. Integrate generated-frame timestamps and 45→90 pacing.
5. Reset every temporal resource on pause, reconnect, codec change, or stream
   generation change.
6. Measure generated frames, fallbacks, pacing misses, and GPU cost.
7. Validate visual output and cadence before enabling the path for games.

## Acceptance markers

Space Warp cannot be called active until logs contain equivalent evidence to:

```text
XR_FB_space_warp advertised=1 enabled=1
Space Warp metadata valid: views=2
```

FSR3 Frame Generation additionally requires:

```text
FSR3 backend initialized: Vulkan
FSR3 inputs valid: views=2
FSR3 generated frame submitted
```

Visual acceptance also requires stable cadence, no old-generation frame or
pose reuse, and no regression when frame generation is disabled.

## Safety rules

- Do not modify or inject into VRChat or another game.
- Do not advertise active synthesis from extension enumeration alone.
- Do not synthesize production depth or motion vectors from color alone.
- Do not submit an incomplete generated image.
- Preserve normal composition when metadata, timing, synchronization, or
  Vulkan validation fails.
- Reset FSR temporal state and request a fresh IDR after reconnection.
- Never mix packets, frames, poses, or history from different connection
  generations.
- Keep installed and build-tree runtimes isolated unless their IPC layouts are
  known to match.
