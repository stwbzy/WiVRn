import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

import launcher
import vr_status


PROJECTION = (
    "  [vr-backend][evidence] projection submitted=120 frame=123 "
    "eye0_image=0x100 eye1_image=0x200 eye0_swapchain_index=2 eye1_swapchain_index=1\n"
)
POSE = (
    "  [vr-device][evidence] pose-applied frame=123 owner=0x300 "
    "pos=(0.0000,1.0000,-0.1000) ori=(0.0000,0.0000,0.0000,1.0000) "
    "hands=1/1 input=1\n"
)


class EvidenceTests(unittest.TestCase):
    def test_projection_and_controller_evidence(self):
        result = vr_status.summarize([
            "  [vr-input] actions attached to session (generation 1)\n",
            "  [vr-backend] XR session FOCUSED\n", PROJECTION, POSE,
            "  [vr-backend][evidence] head-pose frame=123 pos=(0,1,0) "
            "ori=(0,0,0,1) source=runtime predicted=123456\n",
        ])
        self.assertEqual(result["projection_submissions"], 120)
        self.assertEqual(result["last_projected_frame"], 123)
        self.assertTrue(result["distinct_eye_resources"])
        self.assertEqual(result["runtime_head_pose_frame"], 123)
        self.assertEqual(result["applied_head_pose_frame"], 123)
        self.assertTrue(result["controller_actions_attached"])
        self.assertTrue(result["left_hand_active"])
        self.assertTrue(result["right_hand_active"])
        self.assertTrue(result["controller_input_active"])
        self.assertTrue(result["visual_confirmation_required"])

    def test_guest_log_and_unknown_prefix_data_are_not_returned(self):
        secret = "do-not-print-this-session-token"
        result = vr_status.summarize([
            f"[Roblox] cookie={secret}\n", f"[vr-device] unknown-event={secret}\n",
            PROJECTION.rstrip() + f" token={secret}\n",
            "x" * 9000 + secret + "\n",
        ])
        self.assertNotIn(secret, json.dumps(result))
        self.assertEqual(result["projection_submissions"], 0)
        self.assertIsNone(result["left_hand_active"])

    def test_same_resource_is_not_stereo_evidence(self):
        for value in ("0x100", "0x0100", "0x0"):
            with self.subTest(value=value):
                result = vr_status.summarize([PROJECTION.replace("0x200", value)])
                self.assertFalse(result["distinct_eye_resources"])

    def test_gles_resources_are_supported(self):
        result = vr_status.summarize([
            "[vr-backend][evidence] projection submitted=1 frame=2 "
            "graphics=GLES eye0_texture=01 eye1_texture=2\n",
        ])
        self.assertEqual(result["projection_submissions"], 1)
        self.assertTrue(result["distinct_eye_resources"])

    def test_session_teardown_clears_controller_state(self):
        result = vr_status.summarize([
            "[vr-input] actions attached to session (generation 1)\n", POSE, PROJECTION,
            "[vr-backend] XR session torn down (session loss pending)\n",
        ])
        self.assertEqual(result["session_state"], "TORN_DOWN")
        self.assertFalse(result["controller_actions_attached"])
        self.assertFalse(result["controller_input_active"])
        self.assertFalse(result["left_hand_active"])
        self.assertEqual(result["projection_submissions"], 120)

    def test_new_process_does_not_report_old_projection_or_input(self):
        result = vr_status.summarize([
            POSE, PROJECTION,
            "[vr-backend] armed: OpenXR instance/system ready; awaiting guest Vulkan device\n",
        ])
        self.assertEqual(result["projection_submissions"], 0)
        self.assertIsNone(result["left_hand_active"])

    def test_private_vr_log_can_be_summarized(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / "state").mkdir()
            log = root / "state/vr.log"
            log.write_text(PROJECTION, encoding="utf-8")
            with patch.object(launcher, "STATE", root):
                result = launcher.vr_health(log)
            self.assertEqual(result["projection_submissions"], 120)
            self.assertGreaterEqual(result["log_age_seconds"], 0)

    def test_health_refuses_to_read_auth_files(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / "auth").mkdir()
            private = root / "auth/session"
            private.write_text("never read this", encoding="utf-8")
            with patch.object(launcher, "STATE", root), patch.object(Path, "open") as opened:
                with self.assertRaises(launcher.LauncherError):
                    launcher.vr_health(private)
                opened.assert_not_called()

    def test_home_quality_and_performance_fields_are_allowlisted(self):
        result = vr_status.summarize([
            "[vr-home] phase=home recenter=enabled\n",
            "[vr-home] recentered hold_ms=1500 frame=20\n",
            "[vr-quality] preset=balanced scale=1.25 msaa=2 eye=1832x1920\n",
            "[vr-perf] window=10.0s context=xr\n",
            "[vr-perf] counter=presents               total=720 rate=72.00/s\n",
            "[vr-perf] stage=xr_wait_frame    n=720 avg=8.00ms p50=7.00ms p95=9.00ms p99=10.00ms max=12.00ms\n",
            "[vr-perf] counter=unknown-secret total=999 rate=99.00/s\n",
        ])
        self.assertTrue(result["home_recenter_enabled"])
        self.assertEqual(result["home_recenter_frame"], 20)
        self.assertEqual(result["eye_extent"], [1832, 1920])
        self.assertEqual(result["requested_msaa"], 2)
        self.assertEqual(result["perf_counters"], {"presents": 720})
        self.assertEqual(result["pc_stage_timings_ms"]["xr_wait_frame"]["p95"], 9)
        self.assertNotIn("unknown-secret", json.dumps(result))

    def test_sampling_distinguishes_pc_presents_and_xr_submissions(self):
        before = vr_status.empty_status()
        before.update(projection_submissions=120, perf_window_seconds=10,
                      perf_counters={"presents": 600, "frames_submitted": 500})
        after = vr_status.empty_status()
        after.update(projection_submissions=600, perf_window_seconds=20,
                     perf_counters={"presents": 1320, "frames_submitted": 1140})
        with patch.object(launcher, "vr_health", side_effect=[before, after]), \
                patch.object(launcher.time, "sleep"), \
                patch.object(launcher.time, "monotonic", side_effect=[0, 10]):
            result = launcher.sample_health(Path("unused"), 10)
        self.assertEqual(result["pc_present_fps"], 72)
        self.assertEqual(result["pc_projection_fps"], 64)
        self.assertTrue(result["projection_advancing"])


if __name__ == "__main__":
    unittest.main()
