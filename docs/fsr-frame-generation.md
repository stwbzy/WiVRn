# FSR 1 spatial upscaling (stable path)

Status date: 2026-08-10.

The supported experimental graphics feature in the current build is FSR 1
spatial reconstruction: AMD FidelityFX EASU followed by RCAS. It processes
each application frame independently, so it does not change OpenXR pacing,
frame IDs, timestamps, decoder cadence or reconnect behavior.

The implementation is based on AMD's MIT-licensed
[FidelityFX-FSR source](https://github.com/GPUOpen-Effects/FidelityFX-FSR).
It is not AMD FidelityFX FSR 3 and does not synthesize frames.

## Current status

- The headset negotiates a reduced render size and its native recommended
  per-eye output size separately.
- EASU reconstructs the reduced render to the native size, then RCAS sharpens
  it before WiVRn foveation and encoding.
- The compositor has no temporal FSR history; every frame starts from the
  current application image.
- Pause, reconnect and resume use the normal compositor path and request a
  fresh encoder IDR.
- The unstable frame-generation compositor remains in the source tree for
  future repair, but is disabled in normal builds and cannot be enabled by
  runtime environment variables.

The unit suite covers connection lifecycle and FSR dimension/sharpness
configuration. EASU and RCAS shaders are compiled for Vulkan 1.3 and pass
`spirv-val`. Physical visual testing is still required before making a broad
performance or quality claim.

## Configuration

| Variable | Default | Purpose |
| --- | --- | --- |
| `WIVRN_FSR_UPSCALING` | `0` | Enable EASU + RCAS when the headset render resolution is below 100%. |
| `WIVRN_FSR_SHARPNESS` | `0.2` | RCAS sharpness stops, clamped to `0`–`2`; lower values are sharper. |

The old `WIVRN_FSR_FRAME_GENERATION_*` variables are intentionally ignored by
the default build. The server logs a warning if `WIVRN_FSR_FRAME_GENERATION`
is present, then continues with normal composition. To experiment with the
unfinished implementation in a private development build, configure CMake
with `-DWIVRN_ENABLE_EXPERIMENTAL_FRAME_GENERATION=ON`; that path is not part
of the stable FSR test configuration.

FSR needs a lower-resolution input. Set headset **Render resolution** to
70–85% before connecting. At 100%, the server keeps the native path and logs
that FSR is inactive. The implementation also rejects a scale outside FSR 1's
documented 1x–4x output-area range.

## Build and unit tests

```bash
cd /home/stwby/Proyectos/WiVRn-fsr

cmake --preset server-no-system-boost \
  -DWIVRN_BUILD_TEST=ON \
  -DWIVRN_ENABLE_EXPERIMENTAL_FRAME_GENERATION=OFF \
  -DEigen3_DIR=/home/stwby/Proyectos/deps/eigen-install/share/eigen3/cmake \
  -DCMAKE_PREFIX_PATH=/home/stwby/Proyectos/deps/local

cmake --build build-server-no-system-boost -j2
ctest --test-dir build-server-no-system-boost --output-on-failure
```

The expected result is `100% tests passed` for `connection-lifecycle` and
`fsr-config`. The matching Android client can be built separately, but frame
generation-specific decoder timestamp changes are not required for this FSR
only test.

## FSR-only run

Set the headset render resolution to 70–85%, then start the matching server:

```bash
XRT_PRINT_OPTIONS=1 XRT_LOG=info \
WIVRN_FSR_UPSCALING=1 \
WIVRN_FSR_SHARPNESS=0.2 \
build-server-no-system-boost/server/wivrn-server \
  --no-fork --early-active-runtime
```

Expected marker:

```text
FSR 1 spatial upscaling enabled: EASU <render-width>x<render-height> -> <native-width>x<native-height> per eye, RCAS sharpness=0.20 stops
```

The exact dimensions depend on the headset. If the log reports that FSR is
inactive, lower **Render resolution** and reconnect. Compare text and diagonal
edges against a control run with both variables unset. Increase sharpness
toward `2` if edges shimmer or look oversharpened.

## Control and stability checks

Control path:

```bash
env -u WIVRN_FSR_UPSCALING -u WIVRN_FSR_SHARPNESS \
  build-server-no-system-boost/server/wivrn-server \
  --no-fork --early-active-runtime
```

FSR-only path:

```bash
WIVRN_FSR_UPSCALING=1 WIVRN_FSR_SHARPNESS=0.2 \
  build-server-no-system-boost/server/wivrn-server \
  --no-fork --early-active-runtime
```

For this phase, do not set any `WIVRN_FSR_FRAME_GENERATION_*` variable and do
not enable headset Application SpaceWarp. Both control and FSR-only runs must
keep the same application and stream cadence. Check for:

- no black screen at startup or after reconnect;
- contiguous decoded frame IDs and normal timestamps;
- no unexpected `30/60`, `45/90` or `180 Hz` cadence changes;
- successful pause/resume with a fresh IDR;
- no stale frames, decoder errors or compositor exceptions.

## Reconnection behavior

FSR is spatial and has no temporal state to carry across a connection
generation. On pause or link loss the normal compositor clears pending encode
requests, resets the pacer and encoder, and drops old frame timing. Resume
starts with a fresh parameter set and IDR.

## Future frame-generation work

The previous color-history/optical-flow and 45-to-90 path caused black-screen
and cadence failures in combined testing. It is deliberately excluded from
the default build while the following are repaired: image-layout ownership,
candidate lifetime, double-rate pacing, decoder timestamp handling and
reconnect reset ordering. It should be re-enabled only in an isolated
development build after dedicated unit, harness and physical headset tests.
