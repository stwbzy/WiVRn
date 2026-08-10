# Building

# Server (PC)

## Compile

From your checkout directory, with automatic detection of encoders
```bash
cmake -B build-server . -GNinja -DWIVRN_BUILD_CLIENT=OFF -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build-server
```

It is possible to disable specific encoders, by adding options
```
-DWIVRN_USE_NVENC=OFF
-DWIVRN_USE_VAAPI=OFF
-DWIVRN_USE_VULKAN_ENCODE=OFF
-DWIVRN_USE_X264=OFF
```

Force specific audio backends
```
-DWIVRN_USE_PIPEWIRE=ON
```

Systemd service and pretty hostname support
```
-DWIVRN_USE_SYSTEMD=ON
```

Lighthouse driver support for use with lighthouse-tracked devices
```
-DWIVRN_FEATURE_STEAMVR_LIGHTHOUSE=ON
```

Additionally, if your environment requires absolute paths inside the OpenXR runtime manifest, you can add `-DWIVRN_OPENXR_MANIFEST_TYPE=absolute` to the build configuration.

## Profiling / tracing build

To build the server with Perfetto tracing for local profiling, use the `server-tracing` preset (it is the `server` preset plus `WIVRN_USE_PERFETTO=ON`):

```bash
cmake --preset server-tracing
cmake --build build-server-tracing
```

This requires the Perfetto amalgamated SDK installed where CMake looks for it (`/usr/share/perfetto/sdk/perfetto.{h,cc}`, override with `-DPERFETTO_SDK_DIR=<dir>`). Tracing is gated at runtime by the `WIVRN_TRACING` env var, so a tracing-enabled build has no cost until you set it.

To also instrument Monado under the same switch, use the `server-tracing-monado` preset (adds `WIVRN_TRACE_MONADO=ON`); this requires [percetto](https://github.com/olvaffe/percetto/) to be discoverable at configure time.

See [profiling](profiling.md) for how to run the server with tracing and capture/analyse traces.

## Experimental FSR 1 + Frame Generation build

Use the reproducible server preset and keep the generated runtime in the build
tree:

```bash
cmake --preset server-no-system-boost \
  -DWIVRN_BUILD_TEST=ON \
  -DEigen3_DIR=/home/stwby/Proyectos/deps/eigen-install/share/eigen3/cmake \
  -DCMAKE_PREFIX_PATH=/home/stwby/Proyectos/deps/local
cmake --build build-server-no-system-boost -j2
```

Build the matching Android client as well. Double-rate decoding depends on the
advertised stream rate, so an APK from another source revision is not a valid
test client:

```bash
ANDROID_HOME=/home/stwby/Android ./gradlew assembleDebug
adb install -r build/outputs/apk/debug/WiVRn-fsr-debug.apk
```

To combine FSR 1 spatial reconstruction with the color-only 45-to-90 path,
first set the headset **Render resolution** to 70–85%, disable headset
**Application SpaceWarp**, disconnect, then use:

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

This is not AMD FidelityFX FSR 3. It combines a 32-bit Vulkan port of AMD FSR 1
EASU/RCAS with WiVRn's experimental pose-compensated
color-history/optical-flow frame generator. See
[fsr-frame-generation.md](fsr-frame-generation.md) for each switch, expected
logs, fallback behavior, and the physical revalidation checklist.

For an FSR-only smoke test, omit every `WIVRN_FSR_FRAME_GENERATION_*` variable
and keep only `WIVRN_FSR_UPSCALING=1` plus the optional sharpness value.

### OpenXR diagnostic harness

The test build also produces a standalone Vulkan/OpenXR harness:

```bash
cmake --build build-server-no-system-boost \
  --target wivrn-fsr-openxr-harness -j2

XR_RUNTIME_JSON="$PWD/build-server-no-system-boost/openxr_wivrn-dev.json" \
  build-server-no-system-boost/tools/wivrn-fsr-openxr-harness \
  --normal --frames 1
```

Use `--normal --visual-pattern` to exercise the color-only condition used by
VRChat/OpenComposite. Use `--frame-synthesis` or `--space-warp` to submit the
corresponding per-view metadata. The harness remains a diagnostic application;
it does not replace visual and long-duration testing in a real game.

Always pair the build-tree OpenXR manifest and harness with the server from the
same build directory. Do not use `IPC_IGNORE_VERSION=1` to bridge an installed
runtime and an unrelated server: their IPC layouts may differ even if the
version check is bypassed. The variable was used during the structural test
only with matching build-tree components. See
[fsr-frame-generation.md](fsr-frame-generation.md#diagnostic-harness) for the
exact command and interpretation.

# Dashboard

The WiVRn dashboard requires Qt6, and the WiVRn server.

## Compile

From your checkout directory, compile both the server and the dashboard:
```bash
cmake -B build-dashboard . -GNinja -DWIVRN_BUILD_CLIENT=OFF -DWIVRN_BUILD_SERVER=ON -DWIVRN_BUILD_DASHBOARD=ON -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build-dashboard
```

See [Server](#server-pc) for the server compile options.

# Client (headset)

#### Build dependencies
As Arch package names: git pkgconf glslang cmake jdk17-openjdk librsvg cli11 ktx_software-git ([AUR](https://aur.archlinux.org/packages/ktx_software-git))

OpenSSL build dependencies are also needed, as described [here](https://github.com/openssl/openssl/blob/master/INSTALL.md#prerequisites), in particular perl 5.

#### Android environment
Download [sdkmanager](https://developer.android.com/tools/sdkmanager) commandline tool and extract it to any directory.
Create your `ANDROID_HOME` directory, for instance `~/Android`.

Review and accept the licenses with
```bash
sdkmanager --sdk_root="${HOME}/Android" --licenses
```

Install the correct cmake version with
```bash
sdkmanager --sdk_root="${HOME}/Android" --install \
  "cmake;3.31.5" \
  "ndk;29.0.14206865"
```

This checkout pins `android.ndkVersion` to `29.0.14206865` (NDK r29). Gradle
expects that exact side-by-side version under
`$ANDROID_HOME/ndk/29.0.14206865`; having an unrelated NDK on `PATH` is not
sufficient. Verify it before building:

```bash
test -d "$ANDROID_HOME/ndk/29.0.14206865"
```

#### Apk signing
Your device may refuse to install an unsigned apk, so you must create signing keys before building the client
```
# Create key, then enter the password and other information that the tool asks for
keytool -genkey -v -keystore ks.keystore -alias default_key -keyalg RSA -keysize 2048 -validity 10000

# Substitute your password that you entered in the command above instead of YOUR_PASSWORD
echo signingKeyPassword="YOUR_PASSWORD" > gradle.properties
```
Once you have generated the keys, the apk will be automatically signed at build time

#### Client build
From the main directory.
```bash
export ANDROID_HOME=~/Android
export JAVA_HOME=/usr/lib/jvm/java-17-openjdk/

./gradlew assembleDebug
./gradlew assembleRelease
```

Outputs are written below `build/outputs/apk/debug/` and
`build/outputs/apk/release/`. In this fork the local artifacts are:

```text
build/outputs/apk/debug/WiVRn-fsr-debug.apk
build/outputs/apk/release/WiVRn-fsr-release-unsigned.apk
```

The debug artifact uses the Gradle debug key. A release artifact remains
unsigned when no release keystore is configured; do not distribute it until it
has been signed and verified.

#### Install apk with adb
Before using adb you must enable usb debugging on your device:
 * Pico - https://developer.picoxr.com/document/unity-openxr/set-up-the-development-environment/ (see first step)
 * Quest - https://developer.oculus.com/documentation/unity/unity-env-device-setup/#headset-setup (see "Set Up Meta Headset" and "Test an App on Headset" until step 4)

Also add your device in udev rules: https://wiki.archlinux.org/title/Android_Debug_Bridge#Adding_udev_rules

Then connect the device via usb to your computer and execute the following commands
```
# Start adb server
adb start-server

# Check if the device is connected
adb devices

# Install apk
adb install -r build/outputs/apk/debug/WiVRn-fsr-debug.apk

# When you're done, you can stop the adb server
adb kill-server
```

For the lifecycle unit test in a configured server build:

```bash
ctest --test-dir build-server-no-system-boost \
  --output-on-failure -R connection-lifecycle
```
