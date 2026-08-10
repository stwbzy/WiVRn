# Debugging WiVRn

This guide covers debugging techniques for WiVRn, including log collection, crash analysis, performance profiling, and common troubleshooting scenarios.

## Table of Contents

- [Prerequisites](#prerequisites)
- [Collecting Logs](#collecting-logs)
  - [HMD Client Logs](#hmd-client-logs)
  - [Server Logs](#server-logs)
- [Connection Lifecycle](#connection-lifecycle)
- [FSR 1 spatial upscaling](#fsr-1-spatial-upscaling)
- [Virtual Body Trackers](#virtual-body-trackers)
- [Starting Components for Debugging](#starting-components-for-debugging)
  - [HMD Client](#hmd-client)
  - [Server](#server)
  - [XR Applications](#xr-applications)
- [Crash Debugging](#crash-debugging)
  - [Server Crashes](#server-crashes)
  - [HMD Client Crashes](#hmd-client-crashes)
- [Performance Profiling](#performance-profiling)
- [USB Tunneling](#usb-tunneling)
- [Game Debugging](#game-debugging)
- [Environment Variables Reference](#environment-variables-reference)
- [Configuration Files](#configuration-files)
- [External Resources](#external-resources)
- [Remote Native Debugging (Advanced)](#remote-native-debugging-advanced)

---

## Prerequisites

### ADB Setup

The Android Debug Bridge ([ADB](https://developer.android.com/tools/adb)) is required for debugging the HMD client. Connect your headset to your PC via a USB data cable and authorize the connection when prompted in the HMD.

### Clean Installation

When debugging, ensure the HMD client matches the server version. Perform a clean reinstall:

```bash
# Find and uninstall existing installation
WIVRN_PKG=$(adb shell pm list packages | grep wivrn | cut -d: -f2)
adb uninstall ${WIVRN_PKG}

# Install the debug APK
adb install /path/to/wivrn-standard-debug.apk
```

A clean reinstall clears the persisted resume token. Do not uninstall or clear
application data while testing reconnection of an existing PC session. Install
the matching APK in place instead:

```bash
adb install -r build/outputs/apk/debug/WiVRn-fsr-debug.apk
```

Use the clean-install procedure only before starting a new session or when a
deliberately invalid token is part of the test.

---

## Collecting Logs

### HMD Client Logs

The primary method to collect logs from the HMD is through [ADB logcat](https://developer.android.com/tools/logcat).

Watch for crashes (FATAL priority):

```bash
adb logcat '*:F'
```

Record ERROR and above:

```bash
adb logcat '*:E' | tee -a errors-$(date "+%Y-%m-%d-%H").log
```

WiVRn-only logs (verbose):

```bash
adb logcat '*:S' WiVRn:V
```

Watch WiVRn client logs:

```bash
adb logcat | grep WiVRn
```

Watch Android system logs related to WiVRn:

```bash
adb logcat | grep wivrn
```

### Server Logs

Start the server with debug logging enabled:

```bash
XRT_LOG=debug XRT_COMPOSITOR_LOG=debug wivrn-server
```

To also dump video frames for analysis:

```bash
XRT_LOG=debug XRT_COMPOSITOR_LOG=debug WIVRN_DUMP_VIDEO=/tmp/video-dump wivrn-server
```

The video dump can be played back with `mpv` or other media players to verify what video was being sent to the HMD.

There will be multiple files in the directory, one for each video stream per codec. For example, `/tmp/video-dump-0.h264`.

## Connection Lifecycle

The state model, ownership rules, protocol packets, and expected sequences are
specified in [connection-lifecycle.md](connection-lifecycle.md). This section
focuses on field diagnosis.

Every server transition is logged once in this form:

```text
Connection state=reconnecting reason=peer_closed generation=3 previous_duration_ms=81234
```

The states are `listening`, `handshaking`, `streaming`, `reconnecting`, `paused`, `stopping`, and
`failed`. Expected close reasons are `user_requested`, `peer_closed`, `network_timeout`,
`transport_error`, `protocol_error`, `incompatible_resume`, and `server_shutdown`.

The same state is available over D-Bus, without interpreting log text:

```bash
busctl --user get-property io.github.wivrn.Server /io/github/wivrn/Server \
  io.github.wivrn.Server ConnectionState
busctl --user get-property io.github.wivrn.Server /io/github/wivrn/Server \
  io.github.wivrn.Server ConnectionReason
busctl --user get-property io.github.wivrn.Server /io/github/wivrn/Server \
  io.github.wivrn.Server ConnectionGeneration
busctl --user get-property io.github.wivrn.Server /io/github/wivrn/Server \
  io.github.wivrn.Server ConnectionAttempts
busctl --user get-property io.github.wivrn.Server /io/github/wivrn/Server \
  io.github.wivrn.Server SuccessfulReconnections
busctl --user get-property io.github.wivrn.Server /io/github/wivrn/Server \
  io.github.wivrn.Server ReconnectionExpirations
busctl --user get-property io.github.wivrn.Server /io/github/wivrn/Server \
  io.github.wivrn.Server DiscardedPackets
```

`HeadsetConnected` becomes true only after the complete crypto, transport, headset-info and
Monado-session handshake. During an accidental link loss it becomes false while the detailed
state is `reconnecting`. The headset displays its local lobby and a countdown instead of the last
streamed frame.

Useful counters are exposed as `ConnectionAttempts`, `SuccessfulReconnections`,
`ReconnectionExpirations`, and `DiscardedPackets`. A rising discarded-packet count immediately
after resume is expected: it records datagrams rejected from an obsolete connection generation.

To exercise the voluntary paths:

```bash
wivrnctl disconnect --mode pause
wivrnctl disconnect --mode terminate
```

For a link-loss test, interrupt Wi-Fi for less than 30 seconds. A successful run must show
`streaming → reconnecting → handshaking → streaming` with a higher generation. A longer outage
must end in `failed reason=network_timeout` and release the session. `incompatible_resume` means
the reconnect did not present both the paired headset key and the active session token, or its
codec/resolution/FOV/features no longer match.

The headset stores the last completed session token as `resume_token` in the application's private
configuration directory with owner-only permissions. This lets Android recreate the WiVRn process
and still resume, and it does not bind the token to the headset's current IP address. Clearing app
data or deleting that file invalidates a paused session; the server still requires the same paired
headset key, so the token alone is not sufficient to resume it.

Some Meta runtimes keep returning `XR_ERROR_RUNTIME_FAILURE` from `xrWaitFrame` after the network
has already resumed. WiVRn first retries that transient error. If the connection is healthy for two
seconds but the local frame loop remains stuck, it recreates only the Android XR activity through a
short-lived helper process and resumes with the persisted token. The PC OpenXR application remains
alive; logs may therefore show a second `peer_closed → handshaking → streaming` transition and one
additional connection generation. The helper process terminates itself after launching the clean XR
activity.

The physical Quest 3S validation on 2026-08-09 exercised this fallback. The
server first resumed the transport as generation 2; because Meta's local frame
loop stayed failed, the client recreated its XR activity and resumed the same
PC session as generation 3. The final D-Bus snapshot was `streaming`,
`HeadsetConnected=true`, `ConnectionAttempts=3`,
`SuccessfulReconnections=2`, and `ReconnectionExpirations=0`.

With a USB cable attached, a controlled physical cut can be reproduced with:

```bash
adb shell svc wifi disable
# Wait until the server logs state=reconnecting, but less than 30 seconds.
adb shell svc wifi enable
```

Always restore Wi-Fi even if the test fails. A Meta runtime fallback can add a
second resume transition; this is expected only when the log first reports the
stuck OpenXR frame loop.

For repeatable transport faults, run the included proxy and configure the headset to connect to
the proxy port:

```bash
tools/wivrn-fault-proxy.py SERVER_ADDRESS --listen-port 9758
```

Its interactive commands include `drop tcp on`, `drop udp on`, `drop both on`, `delay udp 250`,
`close tcp`, `status`, and the corresponding `off` commands. `--close-tcp-after BYTES` closes a
connection during its handshake. Restarting it with `--upstream-bind ADDRESS` permits testing a
new source address while keeping the resume token and paired headset identity unchanged.

---

## FSR 1 spatial upscaling

Run the local server with `XRT_LOG=info` so the backend markers are visible.
For the FSR-only path, the important sequence is:

```text
FSR 1 spatial upscaling enabled: EASU ... -> ... per eye, RCAS sharpness=... stops
```

Interpret common failures as follows:

| Marker | Meaning |
| --- | --- |
| `FSR 1 ... requested but inactive` | Render resolution is 100%, the APK is mismatched, or the requested scale exceeds the 4x output-area limit. |
| `FSR 1 ... initialization failed` | EASU/RCAS Vulkan resources failed; the compositor keeps a complete linear/foveation fallback. |
| `FSR frame generation is disabled in this build` | An old launch command still contains `WIVRN_FSR_FRAME_GENERATION`; this is expected and the normal compositor remains active. |
| Headset at 120 Hz but server reports application 30 Hz / stream 60 Hz | A headset-side frame-rate feature is still active; remove all server and headset frame-generation settings for this test. |
| Black screen or cadence changes | Stop the server, remove every `WIVRN_FSR_FRAME_GENERATION_*` variable, and restart the FSR-only command. |
| Server `90Hz`, client `100Hz` | The headset APK does not match this server build. |

After a reconnect, there must be no temporal FSR state to restore: the encoder
resets, emits a fresh IDR, and EASU/RCAS processes the next complete frame.
See [fsr-frame-generation.md](fsr-frame-generation.md) for the exact command
and stability checklist.

---

## Virtual Body Trackers

On Quest, enable **Hand and body tracking** in the headset's movement tracking settings. Then open
WiVRn's **Settings → Tracking**, enable **Body tracking**, accept the Meta body-tracking permission
prompt, and select the entries under **Virtual body trackers**.

The tracker selection is saved in the headset configuration and is sent to the server immediately
when it changes during a stream. The server keeps disabled virtual trackers available but untracked,
so they can be enabled again without restarting the OpenXR application. A successful update produces
messages like:

```text
Virtual body tracker mask updated: 0xff
Virtual tracker wivrn-left-knee enabled
```

If **Body tracking** was already enabled before installing a build that requests the Meta permission,
toggle it off and on once from the lobby. If the mask message never appears, verify that the headset
and server were built from the same source revision. If the mask appears but poses remain unavailable,
check the Quest movement tracking setting and collect the WiVRn client log with:

```bash
adb logcat '*:S' WiVRn:V
```

---

## Starting Components for Debugging

### HMD Client

The package name varies by build variant:
- `org.meumeu.wivrn` - for app store builds
- `org.meumeu.wivrn.github` - for GitHub release builds
- `org.meumeu.wivrn.local` - for locally built APK
- `org.meumeu.wivrn.github.testing` - for CI runs
- `org.meumeu.wivrn.github.nightly` - for APK repo

Find your installed package with:

```bash
adb shell pm list packages | grep wivrn
```

Start via ADB:

```bash
WIVRN_PKG=$(adb shell pm list packages | grep wivrn | cut -d: -f2)
adb shell am start ${WIVRN_PKG}/org.meumeu.wivrn.MainActivity
```

Connect to a specific server:

Replace `ADDRESS=localhost` with the address of the server you want to connect to.

```bash
WIVRN_PKG=$(adb shell pm list packages | grep wivrn | cut -d: -f2)
ADDRESS=localhost
adb shell am start -a android.intent.action.VIEW -d "wivrn://${ADDRESS}" ${WIVRN_PKG}
```

Stop the client:

```bash
WIVRN_PKG=$(adb shell pm list packages | grep wivrn | cut -d: -f2)
adb shell am force-stop ${WIVRN_PKG}
```

### Server

Start the server with debug logging:

```bash
XRT_LOG=debug XRT_COMPOSITOR_LOG=debug wivrn-server
```

The server will publish itself on the network via mDNS/Zeroconf for discovery by the HMD client.

### XR Applications

Basic OpenXR application:

```bash
xrgears
```

To run Steam games via OpenVR, make sure to add the Steam launch options presented by the server output or in the dashboard. This is required to configure the OpenVR runtime correctly.

Some games have options to control window behavior. Try adding `-windowed` or `-window -w 1280 -h 1024` to the game launch options to allow moving/resizing the local render window. Each game may have different options, check the game's documentation.

> Note: Games linked to OpenVR require [OpenComposite](https://gitlab.com/znixian/OpenOVR) or [xrizer](https://github.com/Supreeeme/xrizer) to translate calls to OpenXR.

---

## Crash Debugging

### Server Crashes

#### Systemd

Core dumps are managed by [systemd-coredump](https://www.freedesktop.org/software/systemd/man/latest/coredumpctl.html).

View the most recent crash:

```bash
coredumpctl info
```

Start a debugger on the most recent crash:

```bash
coredumpctl -1 debug
```

Inside GDB/LLDB:

```
# Select frame 2
f 2

# View local variables
info locals

# Print backtrace
bt
```

#### Non-Systemd Systems

On systems without systemd-coredump, core dumps are handled by the kernel directly.

Check where core dumps are written:

```bash
cat /proc/sys/kernel/core_pattern
```

To set a simple core pattern:

```bash
sudo sysctl -w kernel.core_pattern=/tmp/core.%e.%p.%t
```

This writes cores to `/tmp/` with the format `core.<executable>.<pid>.<timestamp>`.

Analyze a core dump with GDB:

```bash
gdb /usr/bin/wivrn-server /tmp/core.wivrn-server.12345.1706234567
```

Inside GDB:

```
# Print backtrace
bt

# View all threads
info threads

# Switch to thread 2
thread 2

# Print backtrace for all threads
thread apply all bt
```

### HMD Client Crashes

Copy the traceback from the HMD logs using logcat:

```bash
adb logcat '*:F' | grep -A 50 wivrn
```

---

## Performance Profiling

WiVRn supports timing analysis via CSV dumps and detailed system-wide
profiling with Perfetto. See [docs/profiling.md](profiling.md) for capturing
timing data, profiling the HMD client and server, and comparing the Vulkan,
NVENC, and VAAPI encoder paths.

---

## USB Tunneling

For debugging network issues or testing with a wired connection, you can tunnel WiVRn over USB.

Set up the tunnel and connect:

```bash
WIVRN_PKG=$(adb shell pm list packages | grep wivrn | cut -d: -f2)
adb reverse tcp:9757 tcp:9757
adb shell am start -a android.intent.action.VIEW -d "wivrn+tcp://localhost:9757" ${WIVRN_PKG}
```

You are also able to make this connection through the dashboard.

---

## Game Debugging

### Proton/Wine Games

Enable additional logging for Windows compatibility layers:

```
PROTON_LOG=1 WINEDEBUG=vrclient,openxr,steam %command%
```

### OpenComposite

For games using OpenVR, and you are using [OpenComposite](https://gitlab.com/znixian/OpenOVR) to translate calls to OpenXR. 

The OpenComposite log can be found at `$HOME/.local/state/OpenComposite/logs/opencomposite.log`.

### XRizer

To debug [xrizer](https://github.com/Supreeeme/xrizer), you can set the `RUST_LOG` environment variable to control the log level.

| Variable   | Description                                                                 | Default |
|------------|-----------------------------------------------------------------------------|---------|
| `RUST_LOG` | Configures the logging level for Rust applications. Accepts a comma-separated list of directives in the format `module::path=level`, where `level` can be `error`, `warn`, `info`, `debug`, or `trace`. For example, `RUST_LOG=info,my_crate=debug` sets the global level to `info` and the `my_crate` module to `debug`. | `warn`   |

In xrizer, `RUST_LOG` follows the standard [`env_logger` semantics](https://docs.rs/env_logger/latest/env_logger/#enabling-logging) described above. In addition, xrizer defines the following useful nonstandard logging targets:

| Logging Target | Description |
|-----------------|-------------|
| `openvr_calls` | Logs the name of each OpenVR function as they are called |
| `tracked_property` | Logs the name and device index of each requested tracked device property |

The log is located at `$XDG_STATE_HOME/xrizer/xrizer.txt`, or `$HOME/.local/state/xrizer/xrizer.txt` if `$XDG_STATE_HOME` is not set.

---

## Environment Variables Reference

### Compositor Settings

| Variable | Description | Default |
|----------|-------------|---------|
| `XRT_LOG` | Log level (`trace`, `debug`, `info`, `warn`, `error`) | `warn` |
| `XRT_COMPOSITOR_LOG` | Compositor log level (`trace`, `debug`, `info`, `warn`, `error`) | `info` |
| `OXR_VIEWPORT_SCALE_PERCENTAGE` | Per-application viewport scale (set on application, not server) | `100` |
| `WIVRN_FSR_UPSCALING` | Reconstruct reduced render resolution to the headset-recommended size with FSR 1 EASU + RCAS | `0` |
| `WIVRN_FSR_SHARPNESS` | RCAS sharpness in stops (`0` strongest, range `0`–`2`) | `0.2` |
| `WIVRN_FSR_FRAME_GENERATION*` | Ignored by the default build; frame generation is paused for stability work | — |

### Debugging

| Variable | Description |
|----------|-------------|
| `WIVRN_DUMP_VIDEO` | Path to dump video frames (e.g., `/tmp/video-dump`) |
| `WIVRN_DUMP_TIMINGS` | Path to dump timing CSV (e.g., `/tmp/wivrn-timings.csv`) |
| `WIVRN_LOGLEVEL` | Log level for the native client |
| `WIVRN_AUTOCONNECT` | Auto-connect to the first discovered server |
| `WIVRN_TRACING` | Enable WiVRn Perfetto tracing: `system` / `inprocess`. Requires `WIVRN_USE_PERFETTO=ON` at build time. See [docs/profiling.md](profiling.md). |

### Vulkan

| Variable | Description |
|----------|-------------|
| `VK_INSTANCE_LAYERS` | Vulkan validation layers (e.g., `VK_LAYER_KHRONOS_validation`) |

### OpenXR

| Variable | Description |
|----------|-------------|
| `XR_RUNTIME_JSON` | Path to OpenXR runtime configuration |

See also the [Monado compositor documentation](https://monado.freedesktop.org/getting-started.html#compositor) for additional options.

---

## Configuration Files

### OpenXR Runtime Configuration

Active runtime symlink:

```bash
ls -la $HOME/.config/openxr/1/active_runtime.json
```

Should point to `/usr/share/openxr/1/openxr_wivrn.json` or similar.

Check runtime configuration:

```bash
cat /usr/share/openxr/1/openxr_wivrn.json
```

Example output:

```json
{
    "file_format_version": "1.0.0",
    "runtime": {
        "name": "Monado",
        "library_path": "../../../lib64/wivrn/libopenxr_wivrn.so",
        "MND_libmonado_path": "../../../lib64/wivrn/libmonado_wivrn.so"
    }
}
```

Verify library dependencies:

```bash
pushd /usr/share/openxr/1/
ldd ../../../lib64/wivrn/libopenxr_wivrn.so
popd
```

### WiVRn Server Configuration

See [docs/configuration.md](configuration.md).

---

## External Resources

### Tools and Documentation

- [ADB Documentation](https://developer.android.com/tools/adb) - Android Debug Bridge
- [ADB Logcat](https://developer.android.com/tools/logcat) - Log filtering and analysis
- [coredumpctl](https://www.freedesktop.org/software/systemd/man/latest/coredumpctl.html) - Core dump management
- [Perfetto](https://perfetto.dev/docs/) - System profiling and tracing
- [Perfetto UI](https://ui.perfetto.dev/) - Web-based trace viewer

### Monado/OpenXR

- [Monado Getting Started](https://monado.freedesktop.org/getting-started.html) - Compositor configuration
- [Monado Tracing with Perfetto](https://monado.pages.freedesktop.org/monado/tracing-perfetto.html) - Server-side tracing
- [OpenXR](https://www.khronos.org/openxr/) - Khronos OpenXR standard
- [xrgears](https://github.com/ila-embsys/monado_demo_xrgears) - OpenXR test application

### Gaming

- [OpenComposite](https://gitlab.com/znixian/OpenOVR) - OpenVR to OpenXR translation
- [xrizer](https://github.com/Supreeeme/xrizer) - OpenVR to OpenXR translation layer (Rust)

### Rust

- [env_logger](https://docs.rs/env_logger/latest/env_logger/#enabling-logging) - Rust logging configuration via RUST_LOG

### Android Debugging

- [Android NDK Debugging](https://developer.android.com/ndk/guides/ndk-gdb) - Native debugging with ndk-gdb
- [Android Tracing Quickstart](https://perfetto.dev/docs/quickstart/android-tracing) - Perfetto on Android

---

## Remote Native Debugging (Advanced)

This section covers attaching a native debugger to the HMD client remotely. This requires the Android NDK and familiarity with LLDB/GDB.

Start the client with debugging enabled:

```bash
WIVRN_PKG=$(adb shell pm list packages | grep wivrn | cut -d: -f2)
adb shell am start -D ${WIVRN_PKG}/org.meumeu.wivrn.MainActivity
```

Set up LLDB debugging:

1. Push the LLDB server to the device:

   ```bash
   adb shell "mkdir -p /data/local/tmp/tools"
   adb push "$ANDROID_NDK/toolchains/llvm/prebuilt/linux-x86_64/lib64/clang/*/lib/linux/aarch64/lldb-server" /data/local/tmp/tools/
   ```

2. Start the LLDB server on the device:

   ```bash
   adb shell /data/local/tmp/tools/lldb-server platform --server --listen '*:5039'
   ```

3. Forward the port:

   ```bash
   adb forward tcp:5039 tcp:5039
   ```

4. Connect from your host:

   ```bash
   lldb
   (lldb) platform select remote-android
   (lldb) platform connect connect://localhost:5039
   ```

Resume the app (required for `-D` flag):

```bash
adb forward tcp:12345 jdwp:$(adb shell pidof ${WIVRN_PKG})
jdb -attach localhost:12345
```

See the [Android NDK debugging guide](https://developer.android.com/ndk/guides/ndk-gdb) for more details.
