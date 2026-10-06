/*
 * WiVRn VR streaming
 * Copyright (C) 2022  Guillaume Meunier <guillaume.meunier@centraliens.net>
 * Copyright (C) 2022  Patrick Nicolas <patricknicolas@laposte.net>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <magic_enum.hpp>
#include <netinet/in.h>
#include <openssl/aes.h>
#include <optional>
#include <span>
#include <string>
#include <type_traits>
#include <variant>
#include <vector>
#include <openxr/openxr.h>

#include "connection_lifecycle.h"
#include "packed_quaternion.h"
#include "smp.h"
#include "wivrn_serialization_types.h"

namespace wivrn
{

// Increment when the wire packet schema changes.  The protocol hash also
// contains the schema, but keeping an explicit revision makes intentional
// client/server migrations visible and prevents accidental compatibility.
// FBT settings, reset messages and epoch metadata are part of the wire
// protocol.  Keep the revision in lock-step with those fields: unlike the
// schema hash this number is also useful when diagnosing a mismatched pair of
// endpoints before a packet is exchanged.
static constexpr int protocol_revision = 6;

enum class device_id : uint8_t
{
	HEAD,                         // /user/head
	LEFT_CONTROLLER_HAPTIC,       // /user/hand/left/output/haptic
	RIGHT_CONTROLLER_HAPTIC,      // /user/hand/right/output/haptic
	LEFT_TRIGGER_HAPTIC,          // /user/hand/left/output/haptic_trigger
	RIGHT_TRIGGER_HAPTIC,         // /user/hand/right/output/haptic_trigger
	LEFT_THUMB_HAPTIC,            // /user/hand/left/output/haptic_thumb
	RIGHT_THUMB_HAPTIC,           // /user/hand/right/output/haptic_thumb
	GAMEPAD_HAPTIC_LEFT,          // /user/gamepad/output/haptic_left
	GAMEPAD_HAPTIC_RIGHT,         // /user/gamepad/output/haptic_right
	GAMEPAD_HAPTIC_LEFT_TRIGGER,  // /user/gamepad/output/haptic_left_trigger
	GAMEPAD_HAPTIC_RIGHT_TRIGGER, // /user/gamepad/output/haptic_right_trigger
	LEFT_GRIP,                    // /user/hand/left/input/grip/pose
	LEFT_AIM,                     // /user/hand/left/input/aim/pose
	LEFT_PALM,                    // /user/hand/left/palm_ext/pose
	RIGHT_GRIP,                   // /user/hand/right/input/grip/pose
	RIGHT_AIM,                    // /user/hand/right/input/aim/pose
	RIGHT_PALM,                   // /user/hand/right/palm_ext/pose
	X_CLICK,                      // /user/hand/left/input/x/click
	X_TOUCH,                      // /user/hand/left/input/x/touch
	Y_CLICK,                      // /user/hand/left/input/y/click
	Y_TOUCH,                      // /user/hand/left/input/y/touch
	MENU_CLICK,                   // /user/hand/left/input/menu/click
	LEFT_SQUEEZE_CLICK,           // /user/hand/left/input/squeeze/click
	LEFT_SQUEEZE_FORCE,           // /user/hand/left/input/squeeze/force
	LEFT_SQUEEZE_VALUE,           // /user/hand/left/input/squeeze/value
	LEFT_TRIGGER_CLICK,           // /user/hand/left/input/trigger/click
	LEFT_TRIGGER_VALUE,           // /user/hand/left/input/trigger/value
	LEFT_TRIGGER_TOUCH,           // /user/hand/left/input/trigger/touch
	LEFT_TRIGGER_PROXIMITY,       // /user/hand/left/input/trigger/proximity
	LEFT_TRIGGER_CURL,            // /user/hand/left/input/trigger/curl_fb
	LEFT_TRIGGER_SLIDE,           // /user/hand/left/input/trigger/slide_fb
	LEFT_TRIGGER_FORCE,           // /user/hand/left/input/trigger/force
	LEFT_THUMBSTICK_X,            // /user/hand/left/input/thumbstick/x
	LEFT_THUMBSTICK_Y,            // /user/hand/left/input/thumbstick/y
	LEFT_THUMBSTICK_CLICK,        // /user/hand/left/input/thumbstick/click
	LEFT_THUMBSTICK_TOUCH,        // /user/hand/left/input/thumbstick/touch
	LEFT_THUMBREST_TOUCH,         // /user/hand/left/input/thumbrest/touch
	LEFT_THUMBREST_FORCE,         // /user/hand/left/input/thumbrest/force
	LEFT_THUMB_PROXIMITY,         // /user/hand/left/input/thumb_resting_surfaces/proximity
	LEFT_TRACKPAD_X,              // /user/hand/left/input/trackpad/x
	LEFT_TRACKPAD_Y,              // /user/hand/left/input/trackpad/y
	LEFT_TRACKPAD_CLICK,          // /user/hand/left/input/trackpad/click
	LEFT_TRACKPAD_TOUCH,          // /user/hand/left/input/trackpad/touch
	LEFT_TRACKPAD_FORCE,          // /user/hand/left/input/trackpad/force
	LEFT_STYLUS_FORCE,            // /user/hand/left/input/stylus_fb/force
	LEFT_PINCH_POSE,              // /user/hand/left/input/pinch_ext/pose
	LEFT_PINCH_VALUE,             // /user/hand/left/input/pinch_ext/value
	LEFT_PINCH_READY,             // /user/hand/left/input/pinch_ext/ready_ext
	LEFT_POKE,                    // /user/hand/left/input/poke_ext/pose
	LEFT_AIM_ACTIVATE_VALUE,      // /user/hand/left/input/aim_activate_ext/value
	LEFT_AIM_ACTIVATE_READY,      // /user/hand/left/input/aim_activate_ext/ready_ext
	LEFT_GRASP_VALUE,             // /user/hand/left/input/grasp_ext/value
	LEFT_GRASP_READY,             // /user/hand/left/input/grasp_ext/ready_ext
	A_CLICK,                      // /user/hand/right/input/a/click
	A_TOUCH,                      // /user/hand/right/input/a/touch
	B_CLICK,                      // /user/hand/right/input/b/click
	B_TOUCH,                      // /user/hand/right/input/b/touch
	SYSTEM_CLICK,                 // /user/hand/right/input/system/click
	RIGHT_SQUEEZE_CLICK,          // /user/hand/right/input/squeeze/click
	RIGHT_SQUEEZE_FORCE,          // /user/hand/right/input/squeeze/force
	RIGHT_SQUEEZE_VALUE,          // /user/hand/right/input/squeeze/value
	RIGHT_TRIGGER_CLICK,          // /user/hand/right/input/trigger/click
	RIGHT_TRIGGER_VALUE,          // /user/hand/right/input/trigger/value
	RIGHT_TRIGGER_TOUCH,          // /user/hand/right/input/trigger/touch
	RIGHT_TRIGGER_PROXIMITY,      // /user/hand/right/input/trigger/proximity
	RIGHT_TRIGGER_CURL,           // /user/hand/right/input/trigger/curl_fb
	RIGHT_TRIGGER_SLIDE,          // /user/hand/right/input/trigger/slide_fb
	RIGHT_TRIGGER_FORCE,          // /user/hand/right/input/trigger/force
	RIGHT_THUMBSTICK_X,           // /user/hand/right/input/thumbstick/x
	RIGHT_THUMBSTICK_Y,           // /user/hand/right/input/thumbstick/y
	RIGHT_THUMBSTICK_CLICK,       // /user/hand/right/input/thumbstick/click
	RIGHT_THUMBSTICK_TOUCH,       // /user/hand/right/input/thumbstick/touch
	RIGHT_THUMBREST_TOUCH,        // /user/hand/right/input/thumbrest/touch
	RIGHT_THUMBREST_FORCE,        // /user/hand/right/input/thumbrest/force
	RIGHT_THUMB_PROXIMITY,        // /user/hand/right/input/thumb_resting_surfaces/proximity
	RIGHT_TRACKPAD_X,             // /user/hand/right/input/trackpad/x
	RIGHT_TRACKPAD_Y,             // /user/hand/right/input/trackpad/y
	RIGHT_TRACKPAD_CLICK,         // /user/hand/right/input/trackpad/click
	RIGHT_TRACKPAD_TOUCH,         // /user/hand/right/input/trackpad/touch
	RIGHT_TRACKPAD_FORCE,         // /user/hand/right/input/trackpad/force
	RIGHT_STYLUS_FORCE,           // /user/hand/right/input/stylus_fb/force
	RIGHT_PINCH_POSE,             // /user/hand/right/input/pinch_ext/pose
	RIGHT_PINCH_VALUE,            // /user/hand/right/input/pinch_ext/value
	RIGHT_PINCH_READY,            // /user/hand/right/input/pinch_ext/ready_ext
	RIGHT_POKE,                   // /user/hand/right/input/poke_ext/pose
	RIGHT_AIM_ACTIVATE_VALUE,     // /user/hand/right/input/aim_activate_ext/value
	RIGHT_AIM_ACTIVATE_READY,     // /user/hand/right/input/aim_activate_ext/ready_ext
	RIGHT_GRASP_VALUE,            // /user/hand/right/input/grasp_ext/value
	RIGHT_GRASP_READY,            // /user/hand/right/input/grasp_ext/ready_ext
	EYE_GAZE,                     // /user/eyes_ext/input/gaze_ext/pose
	LEFT_HAND,                    // identify hand tracking
	RIGHT_HAND,                   // identify hand tracking
	BODY,                         // identify body tracking
	FACE,                         // identify face tracking

	// Gamepad, microsoft/xbox_controller profile (/user/gamepad)
	GAMEPAD_MENU_CLICK,             // /user/gamepad/input/menu/click
	GAMEPAD_VIEW_CLICK,             // /user/gamepad/input/view/click
	GAMEPAD_A_CLICK,                // /user/gamepad/input/a/click
	GAMEPAD_B_CLICK,                // /user/gamepad/input/b/click
	GAMEPAD_X_CLICK,                // /user/gamepad/input/x/click
	GAMEPAD_Y_CLICK,                // /user/gamepad/input/y/click
	GAMEPAD_DPAD_DOWN_CLICK,        // /user/gamepad/input/dpad_down/click
	GAMEPAD_DPAD_RIGHT_CLICK,       // /user/gamepad/input/dpad_right/click
	GAMEPAD_DPAD_UP_CLICK,          // /user/gamepad/input/dpad_up/click
	GAMEPAD_DPAD_LEFT_CLICK,        // /user/gamepad/input/dpad_left/click
	GAMEPAD_SHOULDER_LEFT_CLICK,    // /user/gamepad/input/shoulder_left/click
	GAMEPAD_SHOULDER_RIGHT_CLICK,   // /user/gamepad/input/shoulder_right/click
	GAMEPAD_THUMBSTICK_LEFT_CLICK,  // /user/gamepad/input/thumbstick_left/click
	GAMEPAD_THUMBSTICK_RIGHT_CLICK, // /user/gamepad/input/thumbstick_right/click
	GAMEPAD_TRIGGER_LEFT_VALUE,     // /user/gamepad/input/trigger_left/value
	GAMEPAD_TRIGGER_RIGHT_VALUE,    // /user/gamepad/input/trigger_right/value
	GAMEPAD_THUMBSTICK_LEFT_X,      // /user/gamepad/input/thumbstick_left/x
	GAMEPAD_THUMBSTICK_LEFT_Y,      // /user/gamepad/input/thumbstick_left/y
	GAMEPAD_THUMBSTICK_RIGHT_X,     // /user/gamepad/input/thumbstick_right/x
	GAMEPAD_THUMBSTICK_RIGHT_Y,     // /user/gamepad/input/thumbstick_right/y
};

enum class interaction_profile : uint8_t
{
	none,
	khr_simple_controller,
	ext_hand_interaction_ext,
	bytedance_pico_neo3_controller,
	bytedance_pico4_controller,
	bytedance_pico4s_controller,
	bytedance_pico_g3_controller,
	google_daydream_controller,
	hp_mixed_reality_controller,
	htc_vive_controller,
	htc_vive_cosmos_controller,
	htc_vive_focus3_controller,
	htc_vive_pro,
	ml_ml2_controller,
	microsoft_motion_controller,
	microsoft_xbox_controller,
	oculus_go_controller,
	oculus_touch_controller,
	meta_touch_pro_controller,
	meta_touch_plus_controller,
	meta_touch_controller_rift_cv1,
	meta_touch_controller_quest_1_rift_s,
	meta_touch_controller_quest_2,
	yvr_touch_controller_yvr,
	samsung_odyssey_controller,
	valve_index_controller,
};

enum video_codec
{
	h264,
	h265,
	hevc = h265,
	av1,
	raw,
};

// The requested image reconstruction path.  "native" preserves WiVRn's
// established one-to-one compositor path; the remaining modes are opt-in.
enum class upscaling_mode : uint8_t
{
	native,
	meta_compositor,
	sgsr1,
	fsr1,
	nis,
	bilinear,
};

// Server-side, color-only frame synthesis.  This deliberately does not map to
// XR_FB_space_warp: that extension is application space warp and requires
// application-provided motion and depth images.
enum class frame_synthesis_mode : uint8_t
{
	off,
	automatic,
	always,
};

// Selects where a requested color-only frame is synthesized.  `auto_select`
// is serialized as "auto" by the client configuration layer; `auto` cannot be
// used as a C++ enumerator because it is a language keyword.
enum class frame_synthesis_backend : uint8_t
{
	auto_select,
	headset_color,
	server_experimental,
};

// Capabilities advertised by the headset and by the server independently.
// The values are a wire bitmask so a newer endpoint can add capabilities
// without changing the negotiation shape.
enum class frame_synthesis_capability : uint8_t
{
	none = 0,
	headset_color = 1 << 0,
	server_experimental = 1 << 1,
};

constexpr bool has_frame_synthesis_capability(uint8_t capabilities, frame_synthesis_capability capability)
{
	return (capabilities & static_cast<uint8_t>(capability)) != 0;
}

enum class runtime_fallback_reason : uint8_t
{
	none,
	upscale_below_threshold,
	unsupported,
	client_gpu_budget,
	client_missed_intervals,
	optical_flow_deadline,
	encode_or_network_deadline,
	low_confidence,
	scene_cut,
};

// Body tracking is negotiated independently from the body provider selected
// by the headset (body_type below).  The profile controls the stability/
// responsiveness trade-off, rather than selecting a joint set.  These values
// intentionally use small, fixed-width enums: they are persisted in
// settings_changed and appear in status/control packets.
enum class body_tracking_profile : uint8_t
{
	stable,
	balanced,
	responsive,
	custom,
};

// Movement intent is separate from the technical tracking profile above. The
// `current` value is deliberately the default and preserves the existing
// Balanced policy for users who do not opt into adaptive movement handling.
enum class body_motion_profile : uint8_t
{
	current,
	automatic,
	sprint,
	chill,
	flow,
};

enum class body_tracking_fidelity : uint8_t
{
	automatic,
	low,
	high,
};

// Persisted calibration policy.  Runtime state is carried separately in
// body_tracking_calibration_state so a preference is never confused with a
// transient status update.
enum class body_tracking_calibration_mode : uint8_t
{
	automatic,
	manual,
};

// This mirrors the states exposed by XR_META_body_tracking_calibration.
enum class body_tracking_calibration_state : uint8_t
{
	unknown,
	valid,
	calibrating,
	invalid,
};

enum class body_tracking_supervisor : uint8_t
{
	off,
	automatic,
	server,
};

// The supervisor may consume the processed body stream or the raw samples.
// Keeping the path an enum leaves room for another explicitly negotiated path
// without changing the settings packet shape.
enum class body_tracking_supervisor_path : uint8_t
{
	filtered,
	raw,
};

enum class body_tracking_supervisor_state : uint8_t
{
	disabled,
	warming_up,
	tracking,
	holding,
	lost,
	recovering,
};

enum class body_tracking_supervisor_reason : uint8_t
{
	none,
	invalid_pose,
	reordered,
	low_confidence,
	excessive_velocity,
	excessive_angular_velocity,
	dropout,
	calibration,
	reset,
	unsupported,
	internal_error,
};

// The reset scope is deliberately separate from the reset request/ack packet
// direction.  `none` is a safe default for zero-initialized packets.
enum class body_tracking_reset : uint8_t
{
	none,
	calibration,
	tracking,
	pose_alignment,
	all,
};

// Posture assistance is a server-side fusion layer for virtual body trackers.
// It intentionally remains separate from the per-joint supervisor above: the
// supervisor validates individual samples while this mode reasons about the
// whole body and the user's support surface.
enum class body_pose_assist_mode : uint8_t
{
	off,
	automatic,
	manual,
};

enum class body_pose_kind : uint8_t
{
	upright,
	back,
	front,
	left_side,
	right_side,
};

enum class body_surface_mode : uint8_t
{
	floor,
	bed,
};

enum class body_pose_output : uint8_t
{
	virtual_trackers,
	body_and_trackers,
};

// The packet carries a bounded catalog selection, never a filesystem path or
// user-provided JSON. Servers map it to a validated local pose UUID.
enum class body_pose_asset_selection : uint8_t
{
	procedural,
	unity_animation_25,
};

enum class body_pose_state : uint8_t
{
	disabled,
	upright,
	transitioning,
	back,
	front,
	left_side,
	right_side,
	uncertain,
};

struct body_pose_profile
{
	// The profile describes the lower-body shape relative to the support plane.
	// Values are deliberately bounded by both the UI and the server normalizer.
	uint16_t hip_height_mm = 130;
	uint16_t knee_bend_deg = 70;
	uint16_t leg_spread_mm = 180;
	uint16_t foot_spread_mm = 120;
	bool operator==(const body_pose_profile &) const = default;
};

struct body_pose_assist_settings
{
	body_pose_assist_mode mode = body_pose_assist_mode::off;
	body_pose_kind manual_pose = body_pose_kind::back;
	body_surface_mode surface = body_surface_mode::floor;
	body_pose_output output = body_pose_output::virtual_trackers;
	body_pose_asset_selection asset = body_pose_asset_selection::procedural;
	uint16_t bed_height_mm = 450;
	uint16_t transition_ms = 450;
	uint16_t lower_body_influence_permille = 250;
	uint16_t anchor_influence_permille = 800;
	std::array<body_pose_profile, 4> profiles{}; // back, front, left, right
	bool operator==(const body_pose_assist_settings &) const = default;
};

// Short aliases are useful to code that deals with FBT as a protocol feature,
// while the descriptive names above remain the public wire-schema names.
using fbt_profile = body_tracking_profile;
using fbt_fidelity = body_tracking_fidelity;
using fbt_calibration_mode = body_tracking_calibration_mode;
using fbt_calibration_state = body_tracking_calibration_state;
using fbt_reset = body_tracking_reset;
using body_tracking_calibration = body_tracking_calibration_mode;
using body_tracking_path = body_tracking_supervisor_path;

struct body_tracking_settings
{
	body_tracking_profile profile = body_tracking_profile::balanced;
	body_motion_profile motion_profile = body_motion_profile::current;
	body_tracking_fidelity fidelity = body_tracking_fidelity::automatic;
	body_tracking_calibration_mode calibration_mode = body_tracking_calibration_mode::automatic;
	body_tracking_supervisor supervisor = body_tracking_supervisor::off;
	body_tracking_supervisor_path supervisor_path = body_tracking_supervisor_path::filtered;
	uint16_t smoothing_position_permille = 250;
	uint16_t smoothing_rotation_permille = 250;
	uint16_t confidence_threshold_permille = 500;
	uint16_t dropout_hold_ms = 100;
	uint16_t recovery_samples = 3;
	uint16_t prediction_ms = 0;
	std::optional<uint16_t> body_height_mm = {};
	bool floor_assist = false;
	uint16_t micro_motion_threshold_mm = 6;
	body_pose_assist_settings pose_assist{};
};

enum class stream_tab : uint8_t
{
	hidden,
	overlay_only,
	compact,
	stats,
	settings,
	foveation_settings,
	applications,
	application_launcher,
};

struct audio_data
{
	XrTime timestamp;
	std::span<uint8_t> payload;
	data_holder data;
	uint64_t connection_generation = 0;
};

namespace from_headset
{
using body_tracking_profile = wivrn::body_tracking_profile;
using body_motion_profile = wivrn::body_motion_profile;
using body_tracking_fidelity = wivrn::body_tracking_fidelity;
using body_tracking_calibration_mode = wivrn::body_tracking_calibration_mode;
using body_tracking_calibration_state = wivrn::body_tracking_calibration_state;
using body_tracking_calibration = wivrn::body_tracking_calibration_mode;
using body_tracking_supervisor = wivrn::body_tracking_supervisor;
using body_tracking_supervisor_path = wivrn::body_tracking_supervisor_path;
using body_tracking_supervisor_state = wivrn::body_tracking_supervisor_state;
using body_tracking_supervisor_reason = wivrn::body_tracking_supervisor_reason;
using body_tracking_reset = wivrn::body_tracking_reset;
using body_tracking_settings = wivrn::body_tracking_settings;
using body_pose_assist_mode = wivrn::body_pose_assist_mode;
using body_pose_kind = wivrn::body_pose_kind;
using body_surface_mode = wivrn::body_surface_mode;
using body_pose_output = wivrn::body_pose_output;
using body_pose_asset_selection = wivrn::body_pose_asset_selection;
using body_pose_state = wivrn::body_pose_state;
using body_pose_profile = wivrn::body_pose_profile;
using body_pose_assist_settings = wivrn::body_pose_assist_settings;

struct crypto_handshake
{
	uint64_t protocol_version;
	std::string public_key; // In PEM format
	std::string name;
	// Empty on a new session. A reconnect must present the token issued by the
	// server for the active session; the paired public key is checked as well.
	std::string resume_token;
};

struct pin_check_1
{
	crypto::smp::msg1 message;
};

struct pin_check_3
{
	crypto::smp::msg3 message;
};

struct visibility_mask_changed
{
	struct mask
	{
		std::vector<XrVector2f> vertices;
		std::vector<uint32_t> indices;
	};
	static const int num_types = 3; // XrVisibilityMaskTypeKHR values
	using masks = std::array<mask, num_types>;

	masks data;
	uint8_t view_index;
};

enum class face_type : uint8_t
{
	none,
	android,
	fb2,
	htc,
};

enum class body_type : uint8_t
{
	none,
	fb,
	meta,
	bd,
	htc,
};

enum class body_part_mask : uint32_t
{
	chest = 1 << 0,
	left_elbow = 1 << 1,
	right_elbow = 1 << 2,
	hip = 1 << 3,
	left_knee = 1 << 4,
	right_knee = 1 << 5,
	left_foot = 1 << 6,
	right_foot = 1 << 7,
	max,
};

struct settings_changed
{
	float preferred_refresh_rate;
	// for automatic
	float minimum_refresh_rate;

	frame_synthesis_mode frame_synthesis = frame_synthesis_mode::off;
	wivrn::frame_synthesis_backend frame_synthesis_backend = wivrn::frame_synthesis_backend::auto_select;
	uint32_t bitrate_bps;

	// Whether the server should mirror the gamepad to a virtual uinput device;
	// gamepad inputs are always forwarded for the OpenXR path
	bool mirror_gamepad = false;
	// which virtual trackers should be enabled for body tracking
	std::underlying_type_t<body_part_mask> enabled_body_parts;

	// Persisted FBT preferences.  Keeping this as one aggregate gives future
	// settings a stable place in the packet and lets Boost.PFR serialize it
	// without a hand-written trait.
	body_tracking_settings body_tracking = {};
};

struct headset_info_packet
{
	// Recommended per-eye size reported by the headset runtime before the
	// user's render-resolution scale is applied. Spatial upscalers use this as
	// their reconstruction target while OpenXR applications keep rendering at
	// render_eye_width/render_eye_height.
	uint16_t native_eye_width;
	uint16_t native_eye_height;
	// OpenXR layer texture requested by the user.  This is independent from
	// the game render size above and may be reconstructed locally, on the PC,
	// or by the headset compositor depending on upscaling.
	uint16_t target_eye_width;
	uint16_t target_eye_height;
	uint16_t render_eye_width;
	uint16_t render_eye_height;
	uint16_t stream_eye_width;
	uint16_t stream_eye_height;
	upscaling_mode upscaling = upscaling_mode::native;
	bool meta_compositor_available = false;
	// Client-side frame synthesis capabilities.  Zero means that Auto remains
	// disabled; the server backend is available only when explicitly requested.
	uint8_t frame_synthesis_capabilities = 0;
	std::vector<float> available_refresh_rates;

	// runtime configurable settings
	settings_changed settings;

	struct audio_description
	{
		uint8_t num_channels;
		uint32_t sample_rate;
	};
	std::optional<audio_description> speaker;
	std::optional<audio_description> microphone;
	std::array<XrFovf, 2> fov;
	bool hand_tracking;
	bool eye_gaze;
	bool palm_pose;
	bool user_presence;
	bool passthrough;
	face_type face_tracking;
	body_type body_tracking;
	// htc body only
	uint32_t num_generic_trackers;
	std::vector<video_codec> supported_codecs; // from preferred to least preferred
	std::optional<uint8_t> bit_depth;
	std::string system_name;

	// Used for the application list
	std::string language;
	std::string country;
	std::string variant;
};

struct handshake
{
	// Sending this on TCP means connection will be TCP only
};

struct disconnect_request
{
	disconnect_mode mode = disconnect_mode::default_mode;
	connection_close_reason reason = connection_close_reason::user_requested;
};

struct disconnect_ack
{
	disconnect_mode mode = disconnect_mode::default_mode;
};

struct heartbeat
{
	uint64_t connection_generation = 0;
	uint64_t sequence = 0;
	bool stream_channel = false;
};

struct stream_ready
{
	uint64_t connection_generation = 0;
};

enum pose_flags : uint8_t
{
	orientation_valid = 1 << 0,
	position_valid = 1 << 1,
	linear_velocity_valid = 1 << 2,
	angular_velocity_valid = 1 << 3,
	orientation_tracked = 1 << 4,
	position_tracked = 1 << 5
};

inline uint8_t to_pose_flags(XrSpaceLocationFlags location_flags, XrSpaceVelocityFlags velocity_flags = 0)
{
	uint8_t flags{};
	if (location_flags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT)
		flags |= pose_flags::orientation_valid;
	if (location_flags & XR_SPACE_LOCATION_POSITION_VALID_BIT)
		flags |= pose_flags::position_valid;
	if (location_flags & XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT)
		flags |= pose_flags::orientation_tracked;
	if (location_flags & XR_SPACE_LOCATION_POSITION_TRACKED_BIT)
		flags |= pose_flags::position_tracked;

	if (velocity_flags & XR_SPACE_VELOCITY_LINEAR_VALID_BIT)
		flags |= pose_flags::linear_velocity_valid;
	if (velocity_flags & XR_SPACE_VELOCITY_ANGULAR_VALID_BIT)
		flags |= pose_flags::angular_velocity_valid;
	return flags;
}

struct tracking
{
	enum state_flags : uint8_t
	{
		recentered = 1 << 0,
	};

	struct pose
	{
		XrPosef pose;
		XrVector3f linear_velocity;
		XrVector3f angular_velocity;
		device_id device;
		uint8_t flags;
	};

	struct view
	{
		// Relative to XR_REFERENCE_SPACE_TYPE_VIEW
		XrPosef pose;
		XrFovf fov;
	};

	// /user/hand/left, /user/hand/right and /user/gamepad
	std::array<interaction_profile, 3> interaction_profiles;

	XrTime production_timestamp;
	XrTime timestamp;
	XrViewStateFlags view_flags;

	uint8_t state_flags;

	std::array<view, 2> views;
	std::vector<pose> device_poses;

	struct android_face
	{
		std::array<float, XR_FACE_PARAMETER_COUNT_ANDROID> parameters;
		std::array<float, XR_FACE_REGION_CONFIDENCE_COUNT_ANDROID> confidences;
		XrFaceTrackingStateANDROID state;
		XrTime sample_time;
		bool is_calibrated;
		bool is_valid;
	};

	struct fb_face2
	{
		XrTime time;
		std::array<float, XR_FACE_EXPRESSION2_COUNT_FB> weights;
		std::array<float, XR_FACE_CONFIDENCE2_COUNT_FB> confidences;
		bool is_valid;
		bool is_eye_following_blendshapes_valid;
	};

	struct htc_face
	{
		XrTime eye_sample_time;
		XrTime lip_sample_time;
		std::array<float, XR_FACIAL_EXPRESSION_EYE_COUNT_HTC> eye;
		std::array<float, XR_FACIAL_EXPRESSION_LIP_COUNT_HTC> lip;
		bool eye_active;
		bool lip_active;
	};

	std::variant<std::monostate, android_face, fb_face2, htc_face> face;
	uint64_t connection_generation = 0;
};

struct derived_pose
{
	device_id source;
	device_id target;
	XrPosef relation;
	uint64_t connection_generation = 0;
};

struct hand_tracking
{
	enum hand_id : uint8_t
	{
		left,
		right,
	};
	struct pose
	{
		XrVector3f position;
		packed_quaternion orientation;
		XrVector3f linear_velocity;
		XrVector3f angular_velocity;
		// In order to avoid packet fragmentation
		// use 2 less bytes for radius
		uint16_t radius; // 10th of mm
		uint8_t flags;
	};

	XrTime production_timestamp;
	XrTime timestamp;
	hand_id hand;
	std::optional<std::array<pose, XR_HAND_JOINT_COUNT_EXT>> joints;
	uint64_t connection_generation = 0;
};

struct meta_body
{
	struct pose
	{
		XrVector3f position;
		packed_quaternion orientation;
		uint8_t flags;
	};
	struct packed_pose
	{
		struct
		{
			int16_t x, y, z; // 10th of mm relative to root
		} position;
		packed_quaternion orientation;
		uint8_t flags;
	};

	XrTime production_timestamp;
	XrTime timestamp;
	float confidence;
	body_tracking_calibration_state calibration_state = body_tracking_calibration_state::unknown;
	body_tracking_fidelity fidelity = body_tracking_fidelity::automatic;

	struct fb_joints
	{
		pose root;
		// excluding root
		std::array<packed_pose, XR_BODY_JOINT_COUNT_FB - 1> joints;
	};
	struct meta_joints
	{
		pose root;
		// excluding root
		std::array<packed_pose, XR_FULL_BODY_JOINT_COUNT_META - 1> joints;
	};
	std::variant<std::monostate, fb_joints, meta_joints> joints;
	// Increments when the headset's body calibration/tracking origin changes.
	// It lets the server discard samples from the previous body coordinate
	// system while connection_generation protects the transport connection.
	uint64_t body_epoch = 0;
	uint64_t connection_generation = 0;
};

struct meta_body_skeleton
{
	struct fb_skeleton
	{
		std::array<XrBodySkeletonJointFB, XR_BODY_JOINT_COUNT_FB> joints;
	};
	struct meta_skeleton
	{
		std::array<XrBodySkeletonJointFB, XR_FULL_BODY_JOINT_COUNT_META> joints;
	};
	std::variant<fb_skeleton, meta_skeleton> skeleton;
	uint64_t body_epoch = 0;
	uint64_t connection_generation = 0;
};

struct bd_body
{
	struct pose
	{
		XrVector3f position;
		packed_quaternion orientation;
		uint8_t flags;
	};

	bool all_tracked;
	XrTime production_timestamp;
	XrTime timestamp;
	std::array<pose, XR_BODY_JOINT_COUNT_BD> joints;
	uint64_t body_epoch = 0;
	uint64_t connection_generation = 0;
};

struct htc_body
{
	static constexpr size_t max_tracked_poses = 16;
	struct pose
	{
		XrPosef pose;
		XrVector3f linear_velocity;
		XrVector3f angular_velocity;
		uint8_t flags;
	};

	XrTime production_timestamp;
	XrTime timestamp;
	std::array<pose, max_tracked_poses> poses;
	uint64_t connection_generation = 0;
};

// Reports that the headset-side runtime started a new body epoch and asks the
// server to clear its body/filter caches. The request is idempotent for a
// given connection generation; the epoch is also carried by body packets.
struct body_tracking_reset_request
{
	uint64_t request_id = 0;
	body_tracking_reset reset = body_tracking_reset::none;
	uint64_t body_epoch = 0;
	uint64_t connection_generation = 0;
};

struct inputs
{
	struct input_value
	{
		device_id id;
		float value;
		XrTime last_change_time;
	};
	std::vector<input_value> values;
	uint64_t connection_generation = 0;
};

struct hid
{
	struct button_down
	{
		uint8_t button;
	};

	struct button_up
	{
		uint8_t button;
	};

	struct mouse_move
	{
		float x;
		float y;
	};

	struct mouse_scroll
	{
		float h;
		float v;
	};

	struct key_down
	{
		uint8_t key;
	};

	struct key_up
	{
		uint8_t key;
	};

	using input_t = std::variant<button_down, button_up, mouse_move, mouse_scroll, key_down, key_up>;
	struct input
	{
		input_t input_data;
	};
};

struct timesync_response
{
	XrTime query;
	XrTime response;
	uint64_t connection_generation = 0;
};

struct feedback
{
	uint64_t frame_index;
	uint8_t stream_index;

	// Timestamps
	XrTime encode_begin;
	XrTime encode_end;
	XrTime send_begin;
	XrTime send_end;
	XrTime received_first_packet;
	XrTime received_last_packet;
	XrTime sent_to_decoder;
	XrTime received_from_decoder;
	XrTime blitted;
	XrTime displayed;

	uint8_t times_displayed;
	uint64_t connection_generation = 0;
};

struct battery
{
	float charge;
	bool present;
	bool charging;
	uint64_t connection_generation = 0;
};

struct refresh_rate_changed
{
	float from;
	float to;
};

struct session_state_changed
{
	XrSessionState state;
};

struct user_presence_changed
{
	bool present;
};

struct stream_tab_changed
{
	stream_tab tab;
};

struct override_foveation_center
{
	bool enabled;
	float pitch;
	float distance;
};

struct get_application_list
{
	std::string language;
	std::string country;
	std::string variant;
};

struct start_app
{
	std::string app_id;
};

struct get_running_applications
{};

struct set_active_application
{
	uint32_t id;
};

struct stop_application
{
	uint32_t id;
};

// when changing this, also make sure there are handlers in wivrn_session, etc. or compilation will fail
using packets = std::variant<
        crypto_handshake,
        pin_check_1,
        pin_check_3,
        headset_info_packet,
        settings_changed,
        feedback,
        audio_data,
        handshake,
        disconnect_request,
        disconnect_ack,
        heartbeat,
        stream_ready,
        tracking,
        derived_pose,
        hand_tracking,
        meta_body,
        meta_body_skeleton,
        bd_body,
        htc_body,
        body_tracking_reset_request,
        inputs,
        timesync_response,
        battery,
        visibility_mask_changed,
        refresh_rate_changed,
        session_state_changed,
        user_presence_changed,
        stream_tab_changed,
        override_foveation_center,
        get_application_list,
        start_app,
        get_running_applications,
        set_active_application,
        hid::input,
        stop_application>;
} // namespace from_headset

namespace to_headset
{

using body_tracking_profile = wivrn::body_tracking_profile;
using body_motion_profile = wivrn::body_motion_profile;
using body_tracking_fidelity = wivrn::body_tracking_fidelity;
using body_tracking_calibration_mode = wivrn::body_tracking_calibration_mode;
using body_tracking_calibration_state = wivrn::body_tracking_calibration_state;
using body_tracking_calibration = wivrn::body_tracking_calibration_mode;
using body_tracking_supervisor = wivrn::body_tracking_supervisor;
using body_tracking_supervisor_path = wivrn::body_tracking_supervisor_path;
using body_tracking_supervisor_state = wivrn::body_tracking_supervisor_state;
using body_tracking_supervisor_reason = wivrn::body_tracking_supervisor_reason;
using body_tracking_reset = wivrn::body_tracking_reset;
using body_tracking_settings = wivrn::body_tracking_settings;
using body_pose_assist_mode = wivrn::body_pose_assist_mode;
using body_pose_kind = wivrn::body_pose_kind;
using body_surface_mode = wivrn::body_surface_mode;
using body_pose_output = wivrn::body_pose_output;
using body_pose_asset_selection = wivrn::body_pose_asset_selection;
using body_pose_state = wivrn::body_pose_state;
using body_pose_profile = wivrn::body_pose_profile;
using body_pose_assist_settings = wivrn::body_pose_assist_settings;

struct crypto_handshake
{
	enum class crypto_state : uint8_t
	{
		encryption_disabled,
		pin_needed,
		client_already_paired,
		pairing_disabled,
		incompatible_version,
		server_busy,
	};

	std::string public_key; // In PEM format
	crypto_state state;
};

struct pin_check_2
{
	crypto::smp::msg2 message;
};

struct pin_check_4
{
	crypto::smp::msg4 message;
};

struct handshake
{
	// -1 if stream socket should not be used
	int stream_port;
	uint64_t connection_generation;
	std::string resume_token;
};

struct disconnect_request
{
	disconnect_mode mode = disconnect_mode::default_mode;
	connection_close_reason reason = connection_close_reason::user_requested;
};

struct disconnect_ack
{
	disconnect_mode mode = disconnect_mode::default_mode;
};

struct connection_state
{
	server_connection_state state = server_connection_state::listening;
	connection_close_reason reason = connection_close_reason::none;
	uint64_t connection_generation = 0;
	uint32_t retry_remaining_ms = 0;
};

struct heartbeat
{
	uint64_t connection_generation = 0;
	uint64_t sequence = 0;
	bool stream_channel = false;
};

struct server_message
{
	enum class kind : uint8_t
	{
		// in-stream toasts (not buffered)
		toast,
		toast_urgent,

		// displayed in lobby after disconnect (buffered)
		error,
	};

	kind kind;
	std::string msg;
};

struct foveation_parameter
{
	// The number of source pixels for each ratio,
	// the middle one is 1:1
	//
	// how to read it:
	// 1, 4, 5, 3, 1 means:
	// the first output pixel has 3 source pixels
	// the 4 that come after have 2 source pixels
	// then 5 with 1 source pixel
	// 3 with 2 source pixels
	// 1 with 3 source pixels
	std::vector<uint16_t> x;
	std::vector<uint16_t> y;
};

struct audio_stream_description
{
	struct device
	{
		uint8_t num_channels;
		uint32_t sample_rate;
	};
	std::optional<device> speaker;
	std::optional<device> microphone;
};

struct video_stream_description
{
	// dimensions of the video stream per eye
	// alpha is half resolution
	uint16_t width;
	uint16_t height;
	std::array<video_codec, 3> codec; // left, right, alpha
	float frame_rate;
	float refresh_rate;

	bool operator==(const video_stream_description &) const = default;
};

class video_stream_data_shard
{
public:
	inline static const size_t max_payload_size = 1400;
	// Identifier of stream:
	// 0 left
	// 1 right
	// 2 alpha
	uint8_t stream_item_idx;
	// Packets from a superseded socket generation must never reach a decoder.
	uint64_t connection_generation = 0;
	// Counter increased for each frame
	uint64_t frame_idx;
	// Identifier of the shard within the frame
	uint16_t shard_idx;

	// Position information, must be present on first video shard
	struct view_info_t
	{
		// ns in headset time referential
		XrTime display_time;

		std::array<XrPosef, 2> pose;
		std::array<XrFovf, 2> fov;
		std::array<foveation_parameter, 2> foveation;
		// True when the frame contains an alpha channel
		bool alpha;
		// True for a server-generated interval frame.  ATW remains owned by the
		// headset runtime for both real and generated images.
		bool synthetic = false;
	};
	std::optional<view_info_t> view_info;

	// Information about timing, on last video shard
	struct timing_info_t
	{
		XrTime encode_begin;
		XrTime encode_end;
		XrTime send_begin;
		XrTime send_end;
	};
	std::optional<timing_info_t> timing_info;
	// Actual video data, may contain multiple NAL units
	std::span<uint8_t> payload;

	// Container for the data, read payload instead
	data_holder data;
};

struct haptics
{
	device_id id;
	std::chrono::nanoseconds duration;
	float frequency;
	float amplitude;
	uint64_t connection_generation = 0;
};

struct timesync_query
{
	XrTime query;
	uint64_t connection_generation = 0;
};

struct tracking_control
{
	struct sample
	{
		device_id device;
		XrDuration prediction_ns;
	};

	std::vector<sample> pattern;
	XrDuration motions_to_photons;
};

// A reset ACK carries the epoch selected by the runtime. `accepted` is kept
// explicit so a runtime can reject an unsupported scope without inventing a
// new enum value (and without making the packet direction ambiguous).
struct body_tracking_reset_ack
{
	uint64_t request_id = 0;
	body_tracking_reset reset = body_tracking_reset::none;
	bool accepted = false;
	uint64_t body_epoch = 0;
	uint64_t connection_generation = 0;
};

// Periodic/current body state sent to the headset. The enum values are
// descriptive rather than provider-specific; body_type remains the provider
// advertised in headset_info_packet.
struct body_tracking_status
{
	body_tracking_profile profile = body_tracking_profile::balanced;
	body_tracking_fidelity fidelity = body_tracking_fidelity::automatic;
	body_tracking_calibration_state calibration_state = body_tracking_calibration_state::unknown;
	body_tracking_supervisor supervisor = body_tracking_supervisor::off;
	body_tracking_supervisor_state supervisor_state = body_tracking_supervisor_state::disabled;
	body_tracking_supervisor_reason supervisor_reason = body_tracking_supervisor_reason::none;
	bool active = false;
	uint64_t accepted = 0;
	uint64_t rejected = 0;
	uint64_t held = 0;
	uint64_t body_epoch = 0;
	uint64_t connection_generation = 0;
	body_pose_state pose_state = body_pose_state::disabled;
	uint16_t pose_confidence_permille = 0;
	uint16_t pose_anchor_mask = 0;
	uint64_t pose_fallbacks = 0;
};

struct runtime_telemetry
{
	upscaling_mode requested_upscaling = upscaling_mode::native;
	upscaling_mode effective_upscaling = upscaling_mode::native;
	frame_synthesis_mode requested_frame_synthesis = frame_synthesis_mode::off;
	wivrn::frame_synthesis_backend requested_frame_synthesis_backend = wivrn::frame_synthesis_backend::auto_select;
	wivrn::frame_synthesis_backend effective_frame_synthesis_backend = wivrn::frame_synthesis_backend::auto_select;
	// Backends implemented by the server for this connection.  The client
	// advertises its own mask in headset_info_packet above.
	uint8_t frame_synthesis_capabilities = 0;
	bool frame_synthesis_active = false;
	uint16_t render_eye_width = 0;
	uint16_t render_eye_height = 0;
	uint16_t target_eye_width = 0;
	uint16_t target_eye_height = 0;
	uint64_t real_frames = 0;
	uint64_t synthetic_frames = 0;
	float optical_flow_ms = 0;
	float missed_interval_percent = 0;
	runtime_fallback_reason fallback_reason = runtime_fallback_reason::none;
};

struct feature_control
{
	enum feature
	{
		hid_input,
		microphone,
	};
	feature f;
	bool state;
};

struct refresh_rate_change
{
	float hz;
};

struct stream_tab_change
{
	stream_tab tab;
};

struct application_list
{
	std::string language;
	std::string country;
	std::string variant;
	struct application
	{
		std::string id;
		std::string name;
	};
	std::vector<application> applications;
};

struct application_icon
{
	std::string id;
	std::vector<std::byte> image; // In PNG
};

struct running_applications
{
	struct application
	{
		std::string name;
		uint32_t id;
		bool overlay;
		bool active;
	};
	std::vector<application> applications;
};

using packets = std::variant<
        crypto_handshake,
        pin_check_2,
        pin_check_4,
        handshake,
        disconnect_request,
        disconnect_ack,
        connection_state,
        heartbeat,
        server_message,
        audio_stream_description,
        video_stream_description,
        audio_data,
        video_stream_data_shard,
        haptics,
        timesync_query,
        tracking_control,
        body_tracking_reset_ack,
        body_tracking_status,
        runtime_telemetry,
        feature_control,
        refresh_rate_change,
        stream_tab_change,
        application_list,
        application_icon,
        running_applications>;
} // namespace to_headset
} // namespace wivrn

template <>
struct magic_enum::customize::enum_range<wivrn::from_headset::body_part_mask>
{
	static constexpr int min = 0;
	static constexpr int max = static_cast<int>(wivrn::from_headset::body_part_mask::max) - 1;
};
