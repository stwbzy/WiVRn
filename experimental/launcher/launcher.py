#!/usr/bin/env python3
"""Separate Mocktail launcher; no Sober injection, shared credentials or global XR edits."""

import argparse
import fcntl
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import time

import vr_status

ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / "experimental/mocktail-vr"
BUILD = ROOT / "build-roblox-launcher"
DEPS = ROOT / "build-roblox-launcher-deps/root/usr"
STATE = ROOT / "build-roblox-launcher-state"
LOCAL_ID = "3056-3b211f25d8658e88e28c86a4ff1e02ff96092741"
REFERENCE_HASH = "4343a6a900d1fca27ff8e10b9d9c86bee40a5d0ba547cb29903b7769f4f42a9d"
DEFAULT_RUNTIME = Path(
    "/var/lib/flatpak/app/io.github.wivrn.wivrn/current/active/files/share/openxr/1/openxr_wivrn.json"
)
PAYLOAD_ID = re.compile(r"[1-9][0-9]*-[0-9a-f]{40}\Z")


class LauncherError(Exception):
    pass


def regular(path):
    path = Path(path).absolute()
    if path.is_symlink() or not path.is_file():
        raise LauncherError(f"Not a regular file: {path}")
    return path


def document(path):
    path = regular(path)
    if path.stat().st_size > 4 * 1024 * 1024:
        raise LauncherError(f"Metadata too large: {path}")
    with path.open(encoding="utf-8") as stream:
        result = json.load(stream)
    if not isinstance(result, dict):
        raise LauncherError(f"Expected metadata object: {path}")
    return result


def digest(path):
    with regular(path).open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def private_directory(path):
    if path.is_symlink():
        raise LauncherError(f"Private directory cannot be a symlink: {path}")
    path.mkdir(mode=0o700, parents=True, exist_ok=True)
    if not path.is_dir() or path.stat().st_uid != os.getuid():
        raise LauncherError(f"Invalid private directory: {path}")
    path.chmod(0o700)


def environment(state=STATE, inherited=None):
    inherited = os.environ if inherited is None else inherited
    # An allowlist prevents inherited credential, legacy-path and ABI overrides.
    allowed = (
        "DISPLAY", "WAYLAND_DISPLAY", "XDG_RUNTIME_DIR", "DBUS_SESSION_BUS_ADDRESS",
        "PULSE_SERVER", "PIPEWIRE_REMOTE", "LANG", "LANGUAGE", "LC_ALL", "TZ",
        "USER", "LOGNAME",
    )
    env = {name: inherited[name] for name in allowed if name in inherited}
    env["PATH"] = "/usr/bin:/bin"
    env["HOME"] = str(state / "home")
    env["WIVRN_LAUNCHER_SESSION_RUNTIME"] = inherited.get("XDG_RUNTIME_DIR", f"/run/user/{os.getuid()}")
    env["XDG_RUNTIME_DIR"] = f"/run/user/{os.getuid()}"
    for kind in ("config", "data", "cache", "state"):
        env[f"XDG_{kind.upper()}_HOME"] = str(state / kind)
        env[f"MOCKTAIL_{kind.upper()}_ROOT"] = str(state / kind)
    env.update({
        "MOCKTAIL_AUTH_ROOT": str(state / "auth"),
        "MOCKTAIL_PAYLOAD_STORE_ROOT": str(state / "data"),
        "MOCKTAIL_RUNTIME_LIBRARY_DIR": str(BUILD),
        "MOCKTAIL_WORKING_DIRECTORY": str(state / "work"),
        "MOCKTAIL_SKIP_UPDATE_CHECK": "1",
        "MOCKTAIL_VR_ENABLED": "0",
        "MOCKTAIL_USE_REAL_SOBER_PATHS": "0",
        "MOCKTAIL_GRAPHICS_BACKEND": "direct-vulkan",
        "MOCKTAIL_PRELOAD_VULKAN_SHIM": "1",
        "MOCKTAIL_UPDATE_COMPATIBILITY_PATH": str(SOURCE / "config/roblox_compatibility.json"),
        "MOCKTAIL_UPDATE_SIGNING_TRUST_PATH": str(SOURCE / "config/roblox_signing_certificates.json"),
        "MOCKTAIL_UPDATE_HOST_ABI_REFERENCE": str(SOURCE / "config/roblox_host_abi_reference.json"),
        "LD_LIBRARY_PATH": str(DEPS / "lib"),
        "GSETTINGS_BACKEND": "memory",
        "DISABLE_XR_APILAYER_NOV3_xess_upscaler": "1",
    })
    return env


def sandbox(command, env, state=STATE):
    bwrap = shutil.which("bwrap")
    if not bwrap:
        raise LauncherError("bubblewrap is required; refusing an unsandboxed guest launch")
    args = [
        bwrap, "--die-with-parent", "--new-session", "--unshare-pid",
        "--ro-bind", "/", "/", "--proc", "/proc", "--dev-bind", "/dev", "/dev",
        "--tmpfs", "/tmp", "--tmpfs", "/home",
        "--ro-bind", str(ROOT), str(ROOT), "--bind", str(state), str(state),
    ]
    # Keep payloads and derivation inputs immutable even to the guest process.
    for directory in (state / "data/payloads", state / "profiles", state / "reference"):
        if directory.is_dir():
            args += ["--ro-bind", str(directory), str(directory)]
    # Packaged WebKit has fixed libexec paths. Overlay the extracted libraries
    # read-only inside this namespace only, without changing the host /usr.
    args += ["--overlay-src", "/usr/lib", "--overlay-src", str(DEPS / "lib"),
             "--ro-overlay", "/usr/lib"]
    runtime = Path(env.get("WIVRN_LAUNCHER_SESSION_RUNTIME", f"/run/user/{os.getuid()}"))
    if not runtime.is_absolute() or not runtime.is_dir() or runtime.stat().st_uid != os.getuid():
        raise LauncherError("Invalid desktop runtime directory")
    # A private runtime directory receives only the required session sockets.
    # Native instance locks and temporary files never touch the host session.
    destination_runtime = f"/run/user/{os.getuid()}"
    args += ["--bind", str(state / "runtime"), destination_runtime]
    wayland = env.get("WAYLAND_DISPLAY", "")
    names = ["bus", "at-spi", "pipewire-0", "pulse", "wivrn"]
    if wayland:
        if "/" in wayland or wayland in (".", ".."):
            raise LauncherError("WAYLAND_DISPLAY must be a socket name")
        names.append(wayland)
    for name in names:
        if (runtime / name).exists():
            args += ["--ro-bind", str(runtime / name), str(Path(destination_runtime) / name)]
    if Path("/tmp/.X11-unix").is_dir():
        args += ["--ro-bind", "/tmp/.X11-unix", "/tmp/.X11-unix"]
    # Xauthority is not imported; SDL/GTK use the user's existing Wayland socket.
    args += ["--chdir", str(state / "work"), "--"]
    return args + [str(part) for part in command]


def execute(command, env, *, isolated=False, capture=False):
    if isolated:
        command = sandbox(command, env)
    result = subprocess.run([str(part) for part in command], env=env, cwd=STATE / "work",
                            text=True, capture_output=capture)
    if result.returncode:
        if capture and result.stderr:
            print(result.stderr, file=sys.stderr, end="")
        raise LauncherError(f"Command failed with status {result.returncode}")
    return result.stdout.strip() if capture else ""


def payload_path(payload_id, state=STATE):
    if not PAYLOAD_ID.fullmatch(payload_id):
        raise LauncherError("Invalid payload ID")
    path = state / "data/payloads" / payload_id
    if path.is_symlink() or path.resolve() != path.absolute():
        raise LauncherError("Payload cannot escape its private store")
    return path


def active_payload(env):
    payload_id = execute([BUILD / "mocktail_updater", "verify-current"], env, capture=True)
    return payload_id, payload_path(payload_id)


def vr_environment(env, payload_id, payload, runtime):
    runtime = regular(runtime)
    runtime_details = document(runtime).get("runtime")
    if not isinstance(runtime_details, dict) or not isinstance(runtime_details.get("library_path"), str) or not runtime_details["library_path"]:
        raise LauncherError("Invalid OpenXR runtime manifest")
    ipc = Path(env.get("WIVRN_LAUNCHER_SESSION_RUNTIME", f"/run/user/{os.getuid()}")) / "wivrn/comp_ipc"
    if not ipc.is_socket():
        raise LauncherError("WiVRn IPC unavailable; connect the headset first")
    result = dict(env)
    result["XR_RUNTIME_JSON"] = str(runtime)
    result["MOCKTAIL_VR_ENABLED"] = "1"
    result["MOCKTAIL_VR_RUNTIME"] = "wivrn"
    if payload_id == LOCAL_ID:
        # Desktop ABI approval does not replace verification of the VR hooks.
        execute([ROOT / "build-roblox-backend-candidate/wivrn-roblox-contract-verify",
                 payload / "libroblox.so"], env)
        active = document(STATE / "data/current.json")
        relative = active.get("compatibility_manifest_path", "")
        if not isinstance(relative, str) or not relative.startswith("compatibility_profiles/"):
            raise LauncherError("Local VR requires the approved compatibility manifest")
        manifest = STATE / "data" / relative
        if manifest.resolve().parent != (STATE / "data/compatibility_profiles").resolve():
            raise LauncherError("Compatibility manifest escapes its store")
        profiles = document(manifest).get("profiles", [])
        if not any(p.get("elf_build_id") == LOCAL_ID.split("-", 1)[1]
                   and p.get("vr_debug_device_bridge") for p in profiles):
            raise LauncherError("Approved HostAbi manifest has no local VR contract")
        result["WIVRN_ROBLOX_EXPERIMENTAL_BUILD"] = LOCAL_ID.split("-", 1)[1]
    return result


def derive(payload_id, reference, env):
    payload = payload_path(payload_id)
    if digest(reference) != REFERENCE_HASH:
        raise LauncherError("Reference ELF does not match the pinned HostAbi profile")
    output = STATE / "profiles" / payload_id
    if output.is_symlink() or output.resolve().parent != (STATE / "profiles").resolve():
        raise LauncherError("Derivation output escapes the private profile store")
    execute([BUILD / "mocktail_updater", "derive-host-abi", output, regular(reference),
             SOURCE / "config/roblox_host_abi_reference.json", payload,
             SOURCE / "config/roblox_compatibility.json"], env)
    if payload_id == LOCAL_ID:
        execute([ROOT / "build-roblox-backend-candidate/wivrn-roblox-contract-verify",
                 payload / "libroblox.so"], env)
        manifest = document(output / "compatibility.json")
        candidate = document(ROOT / "experimental/backend/installed-candidate.json")
        manifest["profiles"][0]["vr_debug_device_bridge"] = candidate["profiles"][0]["vr_debug_device_bridge"]
        with (output / "compatibility.json").open("w", encoding="utf-8") as stream:
            json.dump(manifest, stream, indent=2)
            stream.write("\n")
    print(f"ABI derived, not approved: {output}")


def vr_health(log):
    path = regular(log)
    if not path.resolve().is_relative_to((STATE / "state").resolve()):
        raise LauncherError("VR evidence log must be inside this launcher's private state directory")
    if path.stat().st_size > 64 * 1024 * 1024:
        raise LauncherError("VR evidence log is too large; rotate it before collecting another sample")
    with path.open(encoding="utf-8", errors="replace") as stream:
        result = vr_status.summarize(stream)
    result["log_age_seconds"] = round(max(0, time.time() - path.stat().st_mtime), 1)
    return result


def configure_vr(env, quality, mirror, diagnostics, audio):
    if quality not in ("balanced", "quality", "performance", "legacy"):
        raise LauncherError("Invalid VR quality preset")
    result = dict(env)
    result.update({
        "WIVRN_ROBLOX_VR_QUALITY": quality,
        "WIVRN_ROBLOX_POINTER": "native",
        "MOCKTAIL_VR_DESKTOP_MIRROR": "1" if mirror else "0",
        "MOCKTAIL_FRAME_RATE_LIMIT": "unlimited",
        "MOCKTAIL_VSYNC": "off",
        "MOCKTAIL_VR_PERF": "1" if diagnostics else "0",
    })
    if quality != "legacy":
        # Leave the user's graphics slider available instead of forcing level 3.
        result["MOCKTAIL_GRAPHICS_QUALITY"] = "auto"
    if audio == "auto":
        pulse_env = {"PATH": "/usr/bin:/bin", "LANG": "C.UTF-8",
                     "HOME": env["HOME"],
                     "XDG_RUNTIME_DIR": env["WIVRN_LAUNCHER_SESSION_RUNTIME"]}
        if "PULSE_SERVER" in env:
            pulse_env["PULSE_SERVER"] = env["PULSE_SERVER"]
        try:
            query = subprocess.run(["/usr/bin/pactl", "--format=json", "list", "sinks"],
                                   capture_output=True, text=True, env=pulse_env, timeout=5)
            sinks = json.loads(query.stdout) if query.returncode == 0 else []
        except (OSError, ValueError, subprocess.TimeoutExpired) as error:
            raise LauncherError("Cannot query WiVRn audio; use --audio desktop explicitly to skip it") from error
        if not isinstance(sinks, list) or not any(
                isinstance(s, dict) and s.get("name") == "wivrn.sink" for s in sinks):
            raise LauncherError("WiVRn audio sink unavailable; connect Quest audio or use --audio desktop")
        result.update({
            "SDL_AUDIO_DRIVER": "pulseaudio",
            "PULSE_SINK": "wivrn.sink",
            "MOCKTAIL_AUDIO_OUTPUT_DEVICE": "WiVRn",
            "SDL_AUDIO_DEVICE_STREAM_NAME": "Roblox VR",
        })
    elif audio != "desktop":
        raise LauncherError("Invalid VR audio mode")
    return result


def sample_health(log, seconds):
    if not 0 <= seconds <= 30:
        raise LauncherError("VR sample interval must be between 0 and 30 seconds")
    before = vr_health(log)
    if not seconds:
        before["projection_advancing"] = None
        return before
    started = time.monotonic()
    time.sleep(seconds)
    after = vr_health(log)
    elapsed = time.monotonic() - started
    delta = after["projection_submissions"] - before["projection_submissions"]
    after["projection_advancing"] = delta > 0
    after["sample_seconds"] = round(elapsed, 2)
    if delta >= 0:
        after["pc_projection_fps_estimate"] = round(delta / elapsed, 1)
        after["fps_estimate_error_bound"] = round(119 / elapsed, 1)
    window = (after.get("perf_window_seconds") or 0) - (before.get("perf_window_seconds") or 0)
    if window > 0:
        for counter, label in (("presents", "pc_present_fps"), ("frames_submitted", "pc_projection_fps")):
            if counter in after["perf_counters"] and counter in before["perf_counters"]:
                diff = after["perf_counters"][counter] - before["perf_counters"][counter]
                if diff >= 0:
                    after[label] = round(diff / window, 1)
    return after


def parser():
    result = argparse.ArgumentParser(description=__doc__)
    sub = result.add_subparsers(dest="command", required=True)
    sub.add_parser("status")
    sub.add_parser("doctor")
    health = sub.add_parser("vr-status", help="Print only allowlisted VR counters, never raw log lines")
    health.add_argument("--log", type=Path, required=True)
    health.add_argument("--sample-seconds", type=float, default=0)
    imported = sub.add_parser("import-apks", help="Verify and stage only; never activate")
    imported.add_argument("base", type=Path)
    imported.add_argument("split", type=Path)
    derived = sub.add_parser("derive")
    derived.add_argument("payload_id")
    derived.add_argument("reference", type=Path)
    canary = sub.add_parser("canary", help="Two desktop canaries, then ABI approval")
    canary.add_argument("payload_id")
    for name in ("desktop", "vr", "probe"):
        launched = sub.add_parser(name)
        if name != "desktop":
            launched.add_argument("--runtime", type=Path, default=DEFAULT_RUNTIME)
        if name == "vr":
            launched.add_argument("--quality", choices=("balanced", "quality", "performance", "legacy"), default="balanced")
            launched.add_argument("--mirror", action="store_true", help="Enable an extra desktop eye copy")
            launched.add_argument("--diagnostics", action="store_true", help="Collect per-stage PC/XR timings")
            launched.add_argument("--audio", choices=("auto", "desktop"), default="auto")
    return result


def main(argv=None):
    options = parser().parse_args(argv)
    os.umask(0o077)
    for path in (STATE, *(STATE / name for name in (
            "home", "config", "data", "cache", "state", "auth", "work", "profiles", "runtime"))):
        private_directory(path)
    lock = os.open(STATE / "launcher.lock", os.O_CREAT | os.O_RDWR | os.O_NOFOLLOW, 0o600)
    if options.command not in ("status", "doctor", "vr-status"):
        try:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError as error:
            os.close(lock)
            raise LauncherError("Another isolated launcher operation is running") from error
    try:
        env = environment()
        if options.command == "doctor":
            required = [BUILD / name for name in (
                "mocktail", "mocktail_updater", "wivrn-launcher-canary", "wivrn-launcher-import")]
            for path in required:
                regular(path)
            execute(["/usr/bin/true"], env, isolated=True)
            print(f"Host built; filesystem sandbox operational; private state: {STATE}")
            print("This does not certify Roblox startup or stereo rendering.")
        elif options.command == "status":
            print(execute([BUILD / "mocktail_updater", "status"], env, capture=True))
        elif options.command == "vr-status":
            print(json.dumps(sample_health(options.log, options.sample_seconds), indent=2))
        elif options.command == "import-apks":
            with tempfile.TemporaryDirectory(prefix="apk-import-", dir=STATE / "cache") as workspace:
                print(execute([BUILD / "wivrn-launcher-import", STATE / "data", workspace,
                               SOURCE / "config/roblox_signing_certificates.json",
                               SOURCE / "config/roblox_compatibility.json",
                               regular(options.base), regular(options.split)], env, capture=True))
            print("Signed payload staged; activation unchanged.")
        elif options.command == "derive":
            derive(options.payload_id, options.reference, env)
        elif options.command == "canary":
            payload_path(options.payload_id)
            profile = STATE / "profiles" / options.payload_id
            if not (profile / "host_abi_profile.json").is_file():
                profile = SOURCE / "config/roblox_compatibility.json"
            execute([BUILD / "wivrn-launcher-canary", STATE / "data", options.payload_id,
                     profile, BUILD / "mocktail", STATE],
                    env, isolated=True)
        elif options.command == "probe":
            env["XR_RUNTIME_JSON"] = str(regular(options.runtime))
            execute([BUILD / "mocktail-vr-probe", "--scene", "--frames", "120"],
                    env, isolated=True)
        else:
            payload_id, payload = active_payload(env)
            if options.command == "vr":
                env = vr_environment(env, payload_id, payload, options.runtime)
                env = configure_vr(env, options.quality, options.mirror, options.diagnostics, options.audio)
                print(f"[isolated-launcher] quality={options.quality} pointer=native audio={options.audio} "
                      "home_recenter=hold-left-menu-1500ms", flush=True)
            execute([BUILD / "mocktail", "--windowed",
                     "--vr" if options.command == "vr" else "--no-vr"], env, isolated=True)
    finally:
        os.close(lock)
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (LauncherError, OSError, ValueError) as error:
        print(f"[isolated-launcher] {error}", file=sys.stderr)
        sys.exit(2)
    except KeyboardInterrupt:
        sys.exit(130)
