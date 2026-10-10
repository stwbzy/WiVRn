# WiVRn FSR Runtime v0.3.1

This release packages the current FSR fork, including the updated headset UI,
body motion profiles and posture assistance. APK and server use protocol
revision 6 and must be updated together.

## Downloads and startup

Install the Quest ARM64 APK with Android platform-tools:

```sh
adb install -r WiVRn-fsr-runtime-v0.3.1-quest-arm64.apk
```

The APK keeps application ID `org.meumeu.wivrn.github` and the previous
release's signing certificate so existing installations can update in place.
It is an optimized, non-debuggable Release build signed with the existing
local Android certificate (the same Android Debug certificate used for v0.3.0).

Run the Linux x86_64 AppImage:

```sh
chmod +x WiVRn-fsr-runtime-v0.3.1-linux-x86_64.AppImage
./WiVRn-fsr-runtime-v0.3.1-linux-x86_64.AppImage --early-active-runtime --no-fork
```

Pairing opens automatically on a PC with no saved headset. Otherwise run
`wivrnctl pair` from the tar.gz package in another terminal while the server
is running. If FUSE is unavailable, prefix the AppImage command with
`APPIMAGE_EXTRACT_AND_RUN=1`.

The tar.gz contains an installed server tree. After extraction, expose its
libraries and data before startup:

```sh
cd WiVRn-v0.3.1
env LD_LIBRARY_PATH="$PWD/usr/lib:$PWD/usr/lib/x86_64-linux-gnu" \
    XDG_DATA_DIRS="$PWD/usr/share:/usr/local/share:/usr/share" \
    ./usr/bin/wivrn-server --early-active-runtime --no-fork
```

OpenVR games still require xrizer or OpenComposite installed on the PC. Use
`PRESSURE_VESSEL_IMPORT_OPENXR_1_RUNTIMES=1 %command%` for Steam games.
Frame synthesis and posture assistance remain experimental and optional.
Compilation and automated tests do not replace a physical headset soak test.

Check downloaded files against the published `SHA256SUMS`.
