"""Summarize allowlisted VR evidence without returning raw guest log content."""

import re


PREFIX = re.compile(r"^\s*\[(vr-backend|vr-device|vr-input|vr-perf|vr-home|vr-quality)\](\[evidence\])? (.*)$")
POINTER = r"0x[0-9a-f]+"
NUMBER_LIST = r"[0-9eE+.,-]+"
PROJECTION = re.compile(
    rf"projection submitted=(\d{{1,20}}) frame=(\d{{1,20}}) "
    rf"eye0_image=({POINTER}) eye1_image=({POINTER}) "
    r"eye0_swapchain_index=\d+ eye1_swapchain_index=\d+"
)
GLES_PROJECTION = re.compile(
    r"projection submitted=(\d{1,20}) frame=(\d{1,20}) "
    r"graphics=GLES eye0_texture=(\d{1,10}) eye1_texture=(\d{1,10})"
)
HEAD_POSE = re.compile(
    rf"head-pose frame=(\d{{1,20}}) pos=\({NUMBER_LIST}\) "
    rf"ori=\({NUMBER_LIST}\) source=runtime predicted=\d+"
)
APPLIED_POSE = re.compile(
    rf"pose-applied frame=(\d{{1,20}}) owner={POINTER} "
    rf"pos=\({NUMBER_LIST}\) ori=\({NUMBER_LIST}\) hands=([01])/([01]) input=([01])"
)


def empty_status():
    return {
        "session_state": "UNKNOWN",
        "controller_actions_attached": False,
        "projection_submissions": 0,
        "last_projected_frame": None,
        "distinct_eye_resources": None,
        "runtime_head_pose_frame": None,
        "applied_head_pose_frame": None,
        "left_hand_active": None,
        "right_hand_active": None,
        "controller_input_active": None,
        "visual_confirmation_required": True,
        "home_recenter_enabled": None,
        "home_recenter_frame": None,
        "render_preset": None,
        "eye_extent": None,
        "requested_msaa": None,
        "perf_window_seconds": None,
        "perf_counters": {},
        "pc_stage_timings_ms": {},
    }


def summarize(lines):
    status = empty_status()
    for line in lines:
        if len(line) > 8192:
            continue
        prefix = PREFIX.fullmatch(line.rstrip("\r\n"))
        if not prefix:
            continue
        component, evidence, message = prefix.groups()
        if component == "vr-home":
            if message in ("phase=home recenter=enabled", "phase=experience recenter=blocked"):
                status["home_recenter_enabled"] = message.startswith("phase=home")
            recenter = re.fullmatch(r"recentered hold_ms=1500 frame=(\d{1,20})", message)
            if recenter:
                status["home_recenter_frame"] = int(recenter[1])
        elif component == "vr-quality":
            quality = re.fullmatch(r"preset=(balanced|quality|performance|legacy) scale=\d\.\d{2} msaa=([124]) eye=(\d{1,5})x(\d{1,5})", message)
            if quality:
                status.update(render_preset=quality[1], requested_msaa=int(quality[2]),
                              eye_extent=[int(quality[3]), int(quality[4])])
        elif component == "vr-perf":
            window = re.fullmatch(r"window=(\d{1,8}\.\d)s context=[a-zA-Z0-9_-]+", message)
            if window:
                status["perf_window_seconds"] = float(window[1])
            counter = re.fullmatch(r"counter=(presents|frames_begun|frames_submitted|frames_skipped)\s+total=(\d{1,20}) rate=\d+\.\d+/s", message)
            if counter:
                status["perf_counters"][counter[1]] = int(counter[2])
            stage = re.fullmatch(
                r"stage=(frame_cycle|xr_wait_frame|copy_fence_wait|copy_submit|mirror_publish|controller_sync)\s+"
                r"n=\d+ avg=(\d+\.\d+)ms p50=(\d+\.\d+)ms p95=(\d+\.\d+)ms p99=\d+\.\d+ms max=\d+\.\d+ms", message)
            if stage:
                status["pc_stage_timings_ms"][stage[1]] = {
                    "average": float(stage[2]), "p50": float(stage[3]), "p95": float(stage[4]),
                }
        if component == "vr-backend" and not evidence:
            if message == "armed: OpenXR instance/system ready; awaiting guest Vulkan device":
                status = empty_status()
            state = re.fullmatch(
                r"XR session (READY|FOCUSED|VISIBLE|STOPPING|EXITING|LOSS_PENDING)(?:;.*| \(.*\))?",
                message,
            )
            if state:
                status["session_state"] = state[1]
                if state[1] in ("STOPPING", "EXITING", "LOSS_PENDING"):
                    status.update(left_hand_active=False, right_hand_active=False,
                                  controller_input_active=False)
            elif message.startswith("XR session not visible (IDLE/SYNCHRONIZED;"):
                status["session_state"] = "NOT_VISIBLE"
                status.update(left_hand_active=False, right_hand_active=False,
                              controller_input_active=False)
            elif message.startswith("XR session torn down ("):
                status["session_state"] = "TORN_DOWN"
                status.update(controller_actions_attached=False, left_hand_active=False,
                              right_hand_active=False, controller_input_active=False)
            elif message == "OpenXR instance loss pending; XR output stopped":
                status["session_state"] = "INSTANCE_LOST"
                status.update(controller_actions_attached=False, left_hand_active=False,
                              right_hand_active=False, controller_input_active=False)
        elif component == "vr-input" and not evidence:
            if re.fullmatch(r"actions attached to session \(generation \d+\)", message):
                status["controller_actions_attached"] = True
        elif component == "vr-backend" and evidence:
            projection = PROJECTION.fullmatch(message) or GLES_PROJECTION.fullmatch(message)
            if projection:
                count, frame, left, right = projection.groups()
                left = int(left, 16 if left.startswith("0x") else 10)
                right = int(right, 16 if right.startswith("0x") else 10)
                status.update(projection_submissions=int(count), last_projected_frame=int(frame),
                              distinct_eye_resources=(left != 0 and right != 0 and left != right))
            head = HEAD_POSE.fullmatch(message)
            if head:
                status["runtime_head_pose_frame"] = int(head[1])
        elif component == "vr-device" and evidence:
            pose = APPLIED_POSE.fullmatch(message)
            if pose:
                status.update(applied_head_pose_frame=int(pose[1]), left_hand_active=pose[2] == "1",
                              right_hand_active=pose[3] == "1", controller_input_active=pose[4] == "1")
    return status
