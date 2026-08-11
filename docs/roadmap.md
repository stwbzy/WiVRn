# FSR spatial stabilization roadmap

Status date: 2026-08-11.

## Implemented in this branch

- Deterministic connection, pause, disconnect, and reconnection lifecycle.
- Configurable PC/headset spatial algorithm, off by default and persisted in `client.json`.
- Native/game/stream/target dimensions plus bitrate mode in the paired protocol.
- Stateless EASU → RCAS → foveated-encoder path with one output per OpenXR frame.
- Encoder dimensions derived from the reconstructed size and aligned safely.
- Normal-composition fallback if FSR initialization fails.
- Post-decode SGSR1, two-pass FSR1, and official NVScaler NIS paths with bilinear fallback and separate GPU timing.
- Removal of server frame generation, temporal history, optical flow, handoff,
  double-rate pacing, decoder timestamp changes, experimental shaders, and
  synthesis-only Monado transport.

## Release gate

- Build and validate EASU/RCAS SPIR-V and run the stereo Vulkan smoke test.
- Build APK and server from the same SHA with Android NDK r29-3.
- Run 100%/off, 80%/off, and two-hour 80%/on physical tests at 90 Hz.
- Complete twenty disconnect/reconnect cycles while monitoring Vulkan errors,
  encoder timeouts, memory, FBT, controllers, and audio.
- Publish an FSR spatial stable pre-release only after the physical matrix
  passes; retain all previous artifacts until then.

## Release status

The Quest 3S physical matrix, repeated reconnection checks, application
launcher validation, and multi-hour VRChat session have passed. The paired
`v0.2.0-fsr-spatial` APK and Linux streamer are approved as a prerelease; the
earlier `v0.1.0-fsr1` preview and its artifacts remain available.
