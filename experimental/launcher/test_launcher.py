import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
from unittest.mock import patch

import launcher


class EnvironmentTests(unittest.TestCase):
    def test_credentials_and_overrides_are_not_inherited(self):
        inherited = {
            "MOCKTAIL_COOKIE_FILE": "/private/cookie",
            "MOCKTAIL_ROBLOX_COOKIES": "secret",
            "MOCKTAIL_USE_REAL_SOBER_PATHS": "1",
            "MOCKTAIL_ALLOW_CANDIDATE_HOST_ABI": "1",
            "MOCKTAIL_SKIP_LIBROBLOX_CTORS": "1",
            "MOCKTAIL_HOST_ABI_PROFILE_FILE": "/unsafe/profile.json",
            "ROBLOX_LIB_PATH": "/unsafe/library.so",
            "LD_PRELOAD": "/unsafe/preload.so",
            "LD_AUDIT": "/unsafe/audit.so",
            "XR_RUNTIME_JSON": "/global/runtime.json",
            "WIVRN_ROBLOX_EXPERIMENTAL_BUILD": "forced",
        }
        env = launcher.environment(inherited=inherited)
        self.assertEqual(env["MOCKTAIL_USE_REAL_SOBER_PATHS"], "0")
        for name in inherited.keys() - {"MOCKTAIL_USE_REAL_SOBER_PATHS"}:
            self.assertNotIn(name, env)

    def test_private_roots(self):
        state = Path("/isolated/state")
        env = launcher.environment(state, {})
        self.assertEqual(env["HOME"], str(state / "home"))
        self.assertEqual(env["MOCKTAIL_AUTH_ROOT"], str(state / "auth"))
        for kind in ("config", "data", "cache", "state"):
            self.assertEqual(env[f"MOCKTAIL_{kind.upper()}_ROOT"], str(state / kind))
            self.assertEqual(env[f"XDG_{kind.upper()}_HOME"], str(state / kind))

    def test_desktop_default_and_explicit_libraries(self):
        env = launcher.environment(inherited={"MOCKTAIL_VR_ENABLED": "1", "PATH": "/unsafe"})
        self.assertEqual(env["MOCKTAIL_VR_ENABLED"], "0")
        self.assertEqual(env["PATH"], "/usr/bin:/bin")
        self.assertEqual(env["LD_LIBRARY_PATH"], str(launcher.DEPS / "lib"))
        self.assertNotIn(str(launcher.BUILD), env["LD_LIBRARY_PATH"].split(":"))

    def test_display_and_audio_session_preserved(self):
        inherited = {"WAYLAND_DISPLAY": "wayland-1", "XDG_RUNTIME_DIR": "/run/user/1000",
                     "DBUS_SESSION_BUS_ADDRESS": "unix:path=/run/user/1000/bus",
                     "PIPEWIRE_REMOTE": "pipewire-0"}
        env = launcher.environment(inherited=inherited)
        for name, value in inherited.items():
            if name != "XDG_RUNTIME_DIR":
                self.assertEqual(env[name], value)
        self.assertEqual(env["WIVRN_LAUNCHER_SESSION_RUNTIME"], inherited["XDG_RUNTIME_DIR"])
        self.assertEqual(env["XDG_RUNTIME_DIR"], f"/run/user/{os.getuid()}")


class PathTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)

    def test_payload_id_cannot_inject_arguments_or_escape(self):
        for value in ("../elsewhere", "3056-abc", "0-" + "a" * 40,
                      "3056-" + "a" * 40 + "/../other", "--force"):
            with self.subTest(value=value), self.assertRaises(launcher.LauncherError):
                launcher.payload_path(value, self.root)

    def test_payload_symlink_rejected(self):
        parent = self.root / "data/payloads"
        parent.mkdir(parents=True)
        (parent / launcher.LOCAL_ID).symlink_to(self.root, target_is_directory=True)
        with self.assertRaises(launcher.LauncherError):
            launcher.payload_path(launcher.LOCAL_ID, self.root)

    def test_metadata_symlink_rejected(self):
        path = self.root / "metadata.json"
        path.write_text("{}", encoding="utf-8")
        link = self.root / "link.json"
        link.symlink_to(path)
        with self.assertRaises(launcher.LauncherError):
            launcher.document(link)

    def test_metadata_requires_object(self):
        path = self.root / "metadata.json"
        path.write_text("[]", encoding="utf-8")
        with self.assertRaises(launcher.LauncherError):
            launcher.document(path)

    def test_private_directory_permissions(self):
        path = self.root / "private"
        launcher.private_directory(path)
        self.assertEqual(path.stat().st_mode & 0o777, 0o700)

    def test_private_directory_symlink_rejected(self):
        path = self.root / "private"
        path.symlink_to(self.root, target_is_directory=True)
        with self.assertRaises(launcher.LauncherError):
            launcher.private_directory(path)

    def test_invalid_reference_rejected_before_derivation(self):
        path = self.root / "reference.so"
        path.write_bytes(b"not the approved ELF")
        with patch.object(launcher, "execute") as execute:
            with self.assertRaises(launcher.LauncherError):
                launcher.derive(launcher.LOCAL_ID, path, {})
            execute.assert_not_called()

    def test_invalid_runtime_rejected_without_starting_guest(self):
        path = self.root / "runtime.json"
        path.write_text('{"runtime": []}', encoding="utf-8")
        with self.assertRaises(launcher.LauncherError):
            launcher.vr_environment({}, launcher.LOCAL_ID, self.root, path)


class SandboxTests(unittest.TestCase):
    def test_no_unsandboxed_fallback(self):
        with patch.object(launcher.shutil, "which", return_value=None):
            with self.assertRaises(launcher.LauncherError):
                launcher.sandbox(["/usr/bin/true"], {})

    def test_home_mask_and_readonly_root(self):
        with patch.object(launcher.shutil, "which", return_value="/usr/bin/bwrap"):
            args = launcher.sandbox(["/usr/bin/true"], {})
        self.assertIn(["--tmpfs", "/home"], [args[n:n+2] for n in range(len(args))])
        self.assertIn(["--ro-bind", "/", "/"], [args[n:n+3] for n in range(len(args))])
        self.assertIn("--ro-overlay", args)
        self.assertNotIn("--not-a-security-boundary", args)

    @unittest.skipUnless(shutil.which("bwrap") and launcher.DEPS.is_dir(), "local sandbox prerequisites")
    def test_real_sandbox_hides_host_home(self):
        with tempfile.TemporaryDirectory(prefix="wivrn-launcher-test-") as temporary:
            state = Path(temporary)
            (state / "work").mkdir()
            (state / "runtime").mkdir()
            env = launcher.environment(state, {})
            command = launcher.sandbox(
                ["/usr/bin/sh", "-c", "test -z \"$(find /home -mindepth 1 -maxdepth 1 -print -quit)\""], env, state)
            self.assertEqual(subprocess.run(command, env=env, capture_output=True).returncode, 0)

    @unittest.skipUnless(shutil.which("bwrap") and launcher.DEPS.is_dir(), "local sandbox prerequisites")
    def test_real_sandbox_cannot_write_payloads(self):
        with tempfile.TemporaryDirectory(prefix="wivrn-launcher-test-") as temporary:
            state = Path(temporary)
            (state / "work").mkdir()
            (state / "runtime").mkdir()
            payloads = state / "data/payloads"
            payloads.mkdir(parents=True)
            marker = payloads / "unexpected-write"
            env = launcher.environment(state, {})
            command = launcher.sandbox(["/usr/bin/touch", str(marker)], env, state)
            result = subprocess.run(command, env=env, capture_output=True)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn(b"Read-only file system", result.stderr)
            self.assertFalse(marker.exists())

    def test_force_flags_not_exposed(self):
        with self.assertRaises(SystemExit), patch("sys.stderr"):
            launcher.parser().parse_args(["vr", "--allow-unverified-build"])


class VrOptionsTests(unittest.TestCase):
    def test_only_guest_audio_is_routed_and_default_sink_is_not_changed(self):
        env = launcher.environment(inherited={})
        reply = subprocess.CompletedProcess([], 0, '[{"name":"wivrn.sink"}]', "")
        with patch.object(launcher.subprocess, "run", return_value=reply) as run:
            result = launcher.configure_vr(env, "balanced", False, True, "auto")
        self.assertEqual(result["PULSE_SINK"], "wivrn.sink")
        self.assertEqual(result["MOCKTAIL_AUDIO_OUTPUT_DEVICE"], "WiVRn")
        self.assertEqual(result["SDL_AUDIO_DRIVER"], "pulseaudio")
        self.assertEqual(result["MOCKTAIL_VR_DESKTOP_MIRROR"], "0")
        self.assertEqual(result["WIVRN_ROBLOX_POINTER"], "native")
        self.assertEqual(result["MOCKTAIL_GRAPHICS_QUALITY"], "auto")
        self.assertEqual(run.call_count, 1)
        self.assertEqual(run.call_args.args[0], ["/usr/bin/pactl", "--format=json", "list", "sinks"])
        self.assertNotIn("PULSE_SINK", env)

    def test_missing_headset_audio_does_not_silently_play_on_desktop(self):
        reply = subprocess.CompletedProcess([], 0, '[]', "")
        with patch.object(launcher.subprocess, "run", return_value=reply):
            with self.assertRaises(launcher.LauncherError):
                launcher.configure_vr(launcher.environment(inherited={}), "balanced", False, False, "auto")

    def test_desktop_audio_is_an_explicit_opt_out(self):
        with patch.object(launcher.subprocess, "run") as run:
            result = launcher.configure_vr(launcher.environment(inherited={}), "legacy", True, False, "desktop")
        run.assert_not_called()
        self.assertNotIn("PULSE_SINK", result)
        self.assertNotIn("MOCKTAIL_AUDIO_OUTPUT_DEVICE", result)
        self.assertNotIn("MOCKTAIL_GRAPHICS_QUALITY", result)

    def test_vr_options_are_bounded(self):
        with self.assertRaises(SystemExit), patch("sys.stderr"):
            launcher.parser().parse_args(["vr", "--quality", "ultra-unbounded"])
        with self.assertRaises(launcher.LauncherError):
            launcher.sample_health(Path("unused"), float("nan"))


if __name__ == "__main__":
    unittest.main()
