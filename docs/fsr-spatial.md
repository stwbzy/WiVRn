# FSR 1 spatial upscaling

WiVRn can reconstruct a reduced per-eye render with AMD FidelityFX FSR 1
EASU, apply RCAS sharpening, and then feed the result to the existing foveated
encoder. This is spatial processing of the current frame only. It is not FSR
3, frame generation, or Application SpaceWarp.

## Enable it

1. Install an APK built from the same commit as the server.
2. Open **Settings → Video** on the headset.
3. Set **Render resolution** to 80% as the initial test point.
4. Enable **FSR 1 spatial upscaling**.
5. Leave **Application SpaceWarp** off and reconnect.

The FSR switch defaults to off, is unavailable at 100% or higher, and is
locked while streaming because it changes negotiated image and encoder sizes.

The server should report a line similar to:

```text
FSR 1 spatial upscaling enabled: 1600x1760 -> 2000x2200 per eye, RCAS 0.20
```

If Vulkan resource or pipeline initialization fails, the server logs a warning
and uses normal composition. It never substitutes an uninitialized image.

## Pipeline and color contract

For every OpenXR frame, the compositor performs exactly one sequence:

```text
layers at render size → EASU to native size → RCAS → foveated encoder → one encoded frame
```

The squasher input is sampled as sRGB data, EASU and RCAS use RGBA16F
intermediates, and RCAS returns linear RGB for the encoder conversion. Explicit
Vulkan barriers separate squashing, EASU, RCAS, foveation, and encoder access.
The encoded dimensions are calculated from the reconstructed native size and
the existing foveation ratio, then aligned for the selected encoder.

FSR has no history, saved poses, optical flow, or cross-connection state. The
server has no candidate-frame, handoff, double-rate, or frame-generation path.

## A/B diagnosis

Keep refresh rate, bitrate, codec, game scene, and Application SpaceWarp
unchanged. Reconnect for each case:

1. 100%, FSR off: native control.
2. 80%, FSR off: reduced-resolution control.
3. 80%, FSR on: spatial reconstruction.

If case 2 is stable and case 3 is not, capture server logs with Vulkan
validation enabled. If both reduced-resolution cases fail, investigate the
render/stream configuration rather than FSR.

## Physical acceptance matrix

- Run 80% with FSR on for at least two hours at 90 Hz.
- Complete twenty disconnect/reconnect cycles.
- Confirm no black frames during start, pause, or reconnect.
- Confirm cadence remains 1:1 and no frame-generation activity exists.
- Check Vulkan validation, encoder timeouts, and memory growth.
- Verify FBT, controllers, audio, disconnect, and reconnect.
- Confirm the Intel Arc A750 does not need its 190 W power limit.

Keep prior artifacts until this matrix passes. Publish only a matching APK and
streamer built from one commit, initially as an FSR spatial stable pre-release.
