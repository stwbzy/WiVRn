/*
 * WiVRn VR streaming
 * Copyright (C) 2024  Guillaume Meunier <guillaume.meunier@centraliens.net>
 * Copyright (C) 2024  Patrick Nicolas <patricknicolas@laposte.net>
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

#include "wivrn_discover.h"
#include "wivrn_packets.h"

#include <filesystem>
#include <map>
#include <mutex>
#include <optional>
#include <simdjson.h>
#include <string>
#include <type_traits>
#include <vector>
#include <openxr/openxr.h>

namespace xr
{
class session;
class system;
} // namespace xr

// key <-> member serialization descriptor, defined in configuration.cpp
struct config_field;

enum class feature
{
	microphone,
	hand_tracking,
	eye_gaze,
	face_tracking,
	body_tracking,
};

class configuration
{
public:
	struct server_data
	{
		bool autoconnect;
		bool manual;
		bool visible;
		bool compatible;

		wivrn_discover::service service;
	};

	std::map<std::string, server_data> servers;
	float preferred_refresh_rate = 0;
	std::optional<float> minimum_refresh_rate;
	// Resolution rendered by the PC application and transported to the client.
	float resolution_scale = 1.0;
	// Resolution of the projection texture submitted to OpenXR.  Keeping this
	// separate lets Horizon OS perform the final layer-to-panel scaling.
	float target_resolution_scale = 1.0;
	wivrn::upscaling_mode upscaling = wivrn::upscaling_mode::native;
	std::optional<wivrn::video_codec> codec;
	uint32_t bitrate_bps = 50'000'000;
	uint8_t bit_depth = 10;

	bool passthrough_enabled = false;
	bool mic_unprocessed_audio = false;

	// Input forwarding, per device. Off by default; only effective if the server permits it.
	bool forward_keyboard = false;
	bool forward_mouse = false;
	bool forward_gamepad = false;

	std::underlying_type_t<wivrn::from_headset::body_part_mask> body_part_mask =
	        static_cast<std::underlying_type_t<wivrn::from_headset::body_part_mask>>(wivrn::from_headset::body_part_mask::max) - 1;

	// Body-tracking policy and filtering preferences.  The legacy body_part_mask
	// above remains separate for compatibility with older client.json files.
	wivrn::body_tracking_settings body_tracking{};

	bool enable_stream_gui = true;

	// application launcher: list vs grid, and grid icon size, 0 small 1 medium 2 large
	bool app_list_view = false;
	uint32_t app_icon_size = 0;

	// Interface defaults match the solid VR system-panel palette. Persisted user
	// choices continue to override these values.
	std::string theme_preset = "Hyprland";
	std::string theme_accent = "Cyan";
	float theme_rounding = 8;
	float theme_card_rounding = 12;
	float theme_font_scale = 1.0;
	float theme_background_alpha = 0.96;

	// XR_FB_composition_layer_settings extension flags
	struct openxr_post_processing_settings
	{
		XrCompositionLayerSettingsFlagsFB super_sampling = 0;
		XrCompositionLayerSettingsFlagsFB sharpening = 0;
	};
	openxr_post_processing_settings openxr_post_processing{};

	std::string virtual_keyboard_layout = "QWERTY";

	std::string environment_model = "assets://ground.glb";

	bool override_foveation_enable = false;
	float override_foveation_pitch = -10 * M_PI / 180;
	float override_foveation_distance = 3;

	bool high_power_mode = true;
	wivrn::frame_synthesis_mode frame_synthesis = wivrn::frame_synthesis_mode::off;
	// Where color-only frame synthesis should run. Auto negotiates the best
	// endpoint capability and never silently enables an unknown backend.
	wivrn::frame_synthesis_backend frame_synthesis_backend = wivrn::frame_synthesis_backend::auto_select;
	// Set only while loading an old client.json.  The old divider path did not
	// generate frames, so it is intentionally migrated to Off instead of being
	// silently re-enabled as runtime ASW.
	bool legacy_fps_divider_migrated = false;
	// Set only when an invalid/legacy backend value was normalized to Auto while
	// loading client.json.  It is intentionally not persisted.
	bool frame_synthesis_backend_migrated = false;
	// Set only while loading a client.json that used body_tracking for both the
	// settings object and the feature flag.  It is intentionally not persisted.
	bool body_tracking_settings_migrated = false;

	// Allow unsafe config values
	bool extended_config = false;

	bool first_run = true;

	std::string locale;

	bool check_feature(feature f) const;
	void set_feature(feature f, bool state);

private:
	mutable std::mutex mutex;
	std::map<feature, bool> features;
	std::optional<float> stream_scale;

	// table of scalar settings shared by save()/load(); non-scalar settings are explicit
	static const std::vector<config_field> & config_fields();

public:
	configuration(xr::system &, xr::session &);
	configuration(xr::system &, xr::session &, const std::filesystem::path &);

	void save();

	void set_stream_scale(float);
	float get_stream_scale() const;
	float get_default_stream_scale() const;

	uint32_t max_bitrate(bool extended) const
	{
		return extended ? 800'000'000u : 200'000'000u;
	}

	uint32_t max_bitrate() const
	{
		return max_bitrate(extended_config);
	}
};
