/*
 * WiVRn VR streaming
 * Copyright (C) 2026  Guillaume Meunier <guillaume.meunier@centraliens.net>
 * Copyright (C) 2026  Patrick Nicolas <patricknicolas@laposte.net>
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

// each page builds a list of `setting` descriptors, render_settings() draws the matching
// ui:: widget for each, rebuilt every frame so dynamic options/descriptions just work
// same pages serve the lobby and the in-stream window, gated by ctx.in_game

#define IMGUI_DEFINE_MATH_OPERATORS
#include "scenes/gui_common.h"

#include "gui_settings.h"

#include "application.h"
#include "configuration.h"
#include "decoder/decoder.h"
#include "render/imgui_impl.h"
#include "render/ui_theme.h"
#include "render/ui_widgets.h"
#include "utils/i18n.h"
#include "xr/instance.h"
#include "xr/session.h"
#include "xr/system.h"

#include "imgui.h"

#include <algorithm>
#include <array>
#include <boost/locale.hpp>
#include <cmath>
#include <functional>
#include <locale>
#include <optional>
#include <spdlog/fmt/fmt.h>
#include <string>
#include <tuple>
#include <vector>

namespace ui = wivrn::ui;

namespace
{
constexpr float control_w = ui::metrics::setting_control_width;

float toggle_width()
{
	return ImGui::GetFrameHeight() * ui::metrics::control_height * ui::metrics::toggle_aspect;
}

enum class ui_kind
{
	toggle,
	slider,
	segmented,
	combo,
	combo_multi,
	button,
};

// one declarative setting, `enabled` greys the row out without hiding it
struct setting
{
	const char * id; // stable imgui id
	std::string label;
	std::string description;
	ui_kind ui = ui_kind::toggle;

	std::function<bool()> get_bool;
	std::function<void(bool)> set_bool;

	std::function<int()> get_int;
	std::function<void(int)> set_int;
	int v_min = 0;
	int v_max = 0;

	std::function<bool(int)> get_multi;
	std::function<void(int, bool)> set_multi;

	std::string fmt;
	std::function<std::vector<std::string>()> options;
	std::string title; // combo modal title

	std::string button_label;       // ui_kind::button
	std::function<void()> on_click; // ui_kind::button

	std::optional<bool> default_bool;               // reset target for toggles
	std::optional<int> default_int;                 // reset target for slider/segmented/combo
	std::optional<std::vector<char>> default_multi; // reset target for multicombo

	std::function<bool()> enabled;
	std::string disabled_tooltip; // shown on a disabled row
};

void render_settings(const wivrn::gui::settings_context & ctx, const char * card_id, const std::vector<setting> & list)
{
	ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ui::metrics::card_item_spacing);
	ui::begin_card(card_id);

	for (const auto & s: list)
	{
		const bool enabled = s.enabled ? s.enabled() : true;
		ImGui::BeginDisabled(not enabled);

		// stable storage for the reset defaults during this call
		bool def_b = s.default_bool.value_or(false);
		int def_i = s.default_int.value_or(0);
		const bool * dpb = s.default_bool ? &def_b : nullptr;
		const int * dpi = s.default_int ? &def_i : nullptr;
		const std::vector<char> * dpm = s.default_multi ? &s.default_multi.value() : nullptr;

		const ImVec2 row_min = ImGui::GetCursorScreenPos();

		const float w = s.ui == ui_kind::toggle ? toggle_width() + (s.default_bool ? ui::reset_slot_width() : 0) : control_w;
		const float label_bottom = ui::setting_label(s.label, s.description, w);

		switch (s.ui)
		{
			case ui_kind::toggle: {
				bool v = s.get_bool();
				if (ui::toggle(s.id, &v, dpb))
					s.set_bool(v);
				break;
			}
			case ui_kind::slider: {
				int v = s.get_int();
				if (ui::slider_int(s.id, &v, s.v_min, s.v_max, s.fmt.c_str(), {control_w, 0}, dpi))
					s.set_int(v);
				break;
			}
			case ui_kind::segmented: {
				const auto opts = s.options();
				int v = s.get_int();
				if (ui::segmented(s.id, opts, &v, {control_w, 0}, dpi))
					s.set_int(v);
				break;
			}
			case ui_kind::combo: {
				const auto opts = s.options();
				std::vector<ui::combo_item> items;
				for (const auto & o: opts)
					items.push_back({o.c_str()});
				int v = s.get_int();
				if (ui::combo(s.id, s.title, items, &v, control_w, dpi))
					s.set_int(v);
				break;
			}
			case ui_kind::combo_multi: {
				const auto opts = s.options();
				std::vector<ui::combo_item> items;
				std::vector<char> selected;
				for (const auto & o: opts)
					items.push_back({o.c_str()});
				for (size_t i = 0; i < items.size(); ++i)
					selected.push_back(s.get_multi(i));

				if (ui::combo_multi(s.id, s.title, s.title, items, &selected, control_w, dpm))
				{
					// TODO only set the changed value
					for (size_t i = 0; i < items.size(); ++i)
						s.set_multi(i, selected[i]);
				}
				break;
			}
			case ui_kind::button: {
				if (ui::button(s.button_label, ui::button_style::secondary, {control_w, 0}) and s.on_click)
					s.on_click();
				break;
			}
		}

		ImGui::EndDisabled();

		// reserve row height for a multi-line description plus padding, Dummy grows content size
		const float pad = std::max(label_bottom - ImGui::GetCursorPosY(), 0.f) + ui::metrics::label_bottom_pad;
		ImGui::Dummy({0, pad});

		// disabled rows can't fire the widget tooltip hook, do it on the row
		if (not enabled and not s.disabled_tooltip.empty())
		{
			const ImVec2 row_max{row_min.x + ImGui::GetContentRegionAvail().x, ImGui::GetCursorScreenPos().y};
			if (ImGui::IsMouseHoveringRect(row_min, row_max))
				ctx.imgui_ctx.tooltip(s.disabled_tooltip, (row_min + row_max) / 2);
		}
	}

	ui::end_card();
	ImGui::PopStyleVar();
}

template <typename Range, typename T>
std::optional<int> index(const Range & range, const T & value)
{
	auto it = std::ranges::find(range, value);
	if (it == range.end())
		return std::nullopt;
	else
		return (it - range.begin());
}

} // namespace

namespace wivrn::gui
{

void settings_video(const settings_context & ctx)
{
	auto & config = ctx.config;
	auto & default_config = ctx.default_config;
	const std::string disconnect_tip = ctx.in_game ? _C("tooltip for disabled settings", "Disconnect to change this setting.") : std::string{};
	std::vector<setting> list;

	if (const auto rates = ctx.session.get_refresh_rates(); not rates.empty())
	{
		int default_rate_index = index(rates, default_config.preferred_refresh_rate).value_or(-1) + 1;

		list.push_back({
		        .id = "##refresh",
		        .label = _("Refresh rate"),
		        .description = _("Use 'auto' to select the refresh rate based on measured application performance. May cause flicker when a change happens."),
		        .ui = ui_kind::segmented,
		        .get_int = [&config, rates] {
			        for (size_t i = 0; i < rates.size(); ++i)
				        if (rates[i] == config.preferred_refresh_rate)
					        return int(i) + 1;
			        return 0; },
		        .set_int = [&ctx, &config, rates](int v) {
			        if (v == 0)
			        {
				        config.preferred_refresh_rate = 0;
			        }
			        else
			        {
				        ctx.session.set_refresh_rate(rates[v - 1]);
				        config.preferred_refresh_rate = rates[v - 1];
			        }
			        config.save();
			        if (ctx.on_streaming_changed)
				        ctx.on_streaming_changed(); },
		        .options = [rates] {
			        std::vector<std::string> opts;
			        opts.push_back(_C("automatic refresh rate, use a short string", "Auto"));
			        for (float r: rates)
				        opts.push_back(fmt::format("{}", int(r)));
			        return opts; },
		        .default_int = default_rate_index,
		});
	}

	{
		const auto width = ctx.recommended_width;
		const auto height = ctx.recommended_height;
		list.push_back({
		        .id = "##resolution",
		        .label = _("Render resolution"),
		        .description = fmt::format(_cF("render resolution", "Pixels rendered per eye ({}x{}). Lower to gain performance."), int(width * config.resolution_scale), int(height * config.resolution_scale)),
		        .ui = ui_kind::slider,
		        .get_int = [&config] { return int(std::lround(config.resolution_scale * 100)); },
		        .set_int = [&config](int v) { config.resolution_scale = v / 100.0; config.save(); },
		        .v_min = 50,
		        .v_max = config.extended_config ? 350 : 150,
		        .fmt = "%d%%",
		        .default_int = int(std::lround(default_config.resolution_scale * 100)),
		        .enabled = [&ctx] { return not ctx.in_game; },
		        .disabled_tooltip = disconnect_tip,
		});
	}

	{
		const auto width = ctx.recommended_width;
		const auto height = ctx.recommended_height;
		list.push_back({
		        .id = "##target_resolution",
		        .label = _("Target resolution"),
		        .description = fmt::format(_cF("target resolution", "OpenXR projection texture per eye ({}x{}). Horizon OS performs the final scale to the physical panel."), int(width * config.target_resolution_scale), int(height * config.target_resolution_scale)),
		        .ui = ui_kind::slider,
		        .get_int = [&config] { return int(std::lround(config.target_resolution_scale * 100)); },
		        .set_int = [&config](int v) { config.target_resolution_scale = v / 100.0; config.save(); },
		        .v_min = 50,
		        .v_max = config.extended_config ? 200 : 150,
		        .fmt = "%d%%",
		        .default_int = int(std::lround(default_config.target_resolution_scale * 100)),
		        .enabled = [&ctx] { return not ctx.in_game; },
		        .disabled_tooltip = disconnect_tip,
		});
	}

	{
		static constexpr std::array modes{
		        wivrn::upscaling_mode::native,
		        wivrn::upscaling_mode::meta_compositor,
		        wivrn::upscaling_mode::sgsr1,
		        wivrn::upscaling_mode::fsr1,
		        wivrn::upscaling_mode::nis,
		        wivrn::upscaling_mode::bilinear,
		};
		const auto profile = application::get_hmd_traits().performance_profile;
		const bool meta_recommended = profile == hmd_performance_profile::quest_legacy;
		list.push_back({
		        .id = "##upscaling",
		        .label = _("Upscaling"),
		        .description = meta_recommended
		                               ? _("Meta compositor is recommended on Quest 1 and Quest 2. Local SGSR1, FSR1 and NIS remain experimental advanced options.")
		                               : _("Select where reconstruction happens. New paths are opt-in; Native keeps the stable one-to-one route."),
		        .ui = ui_kind::combo,
		        .get_int = [&config] { return index(modes, config.upscaling).value_or(0); },
		        .set_int = [&config](int v) { config.upscaling = modes[v]; config.save(); },
		        .options = [meta_recommended] { return std::vector<std::string>{
			                                        _("Native (stable)"),
			                                        meta_recommended ? _("Meta compositor (recommended)") : _("Meta compositor"),
			                                        _("SGSR1 mobile (experimental)"),
			                                        _("FSR1 (experimental)"),
			                                        _("NIS (experimental)"),
			                                        _("Bilinear fused"),
			                                }; },
		        .title = _("Upscaling mode"),
		        .default_int = index(modes, default_config.upscaling).value_or(0),
		        .enabled = [&ctx] { return not ctx.in_game; },
		        .disabled_tooltip = disconnect_tip,
		});
	}

	if (ctx.instance.has_extension(XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME))
	{
		static constexpr std::array modes{
		        wivrn::frame_synthesis_mode::off,
		        wivrn::frame_synthesis_mode::automatic,
		        wivrn::frame_synthesis_mode::always,
		};
		list.push_back({
		        .id = "##frame_synthesis",
		        .label = _("Frame synthesis"),
		        .description = _("Color-only frame synthesis. Auto negotiates the available backend; synthetic frames may still use the normal transport budget."),
		        .ui = ui_kind::segmented,
		        .get_int = [&config] { return index(modes, config.frame_synthesis).value_or(0); },
		        .set_int = [&ctx, &config](int v) { config.frame_synthesis = modes[v]; config.save(); if (ctx.on_streaming_changed) ctx.on_streaming_changed(); },
		        .options = [] { return std::vector<std::string>{_("Off"), _C("automatic frame synthesis", "Auto"), _("Always")}; },
		        .default_int = index(modes, default_config.frame_synthesis).value_or(0),
		});

		static constexpr std::array backends{
		        wivrn::frame_synthesis_backend::auto_select,
		        wivrn::frame_synthesis_backend::headset_color,
		        wivrn::frame_synthesis_backend::server_experimental,
		};
		list.push_back({
		        .id = "##frame_synthesis_backend",
		        .label = _("Synthesis backend"),
		        .description = _("Auto uses headset color synthesis when advertised; otherwise it stays disabled. Select server optical flow explicitly."),
		        .ui = ui_kind::combo,
		        .get_int = [&config] { return index(backends, config.frame_synthesis_backend).value_or(0); },
		        .set_int = [&ctx, &config](int v) {
			        config.frame_synthesis_backend = backends[v];
			        config.save();
			        if (ctx.on_streaming_changed)
				        ctx.on_streaming_changed(); },
		        .options = [] { return std::vector<std::string>{
			                                        _C("frame synthesis backend", "Auto"),
			                                        _C("frame synthesis backend", "Headset color (experimental)"),
			                                        _C("frame synthesis backend", "Server optical flow (experimental)"),
		                                }; },
		        .title = _("Synthesis backend"),
		        .default_int = index(backends, default_config.frame_synthesis_backend).value_or(0),
		});
	}

	if (config.legacy_fps_divider_migrated)
	{
		list.push_back({
		        .id = "##legacy_fps_divider",
		        .label = _("Legacy frame-rate setting disabled"),
		        .description = _("The old half-rate option did not synthesize missing frames. It was migrated to Off and will not be re-enabled automatically."),
		        .ui = ui_kind::button,
		        .button_label = _("Understood"),
		        .on_click = [&config] { config.legacy_fps_divider_migrated = false; },
		});
	}

	ui::page_header(_cS("page header title", "Video"), _cS("page header subtitle", "Frame rate and resolution."));
	render_settings(ctx, "##video", list);
}

void settings_streaming(const settings_context & ctx)
{
	auto & config = ctx.config;
	auto & default_config = ctx.default_config;
	const std::string disconnect_tip = ctx.in_game ? _C("tooltip for disabled settings", "Disconnect to change this setting.") : std::string{};
	std::vector<setting> list;

	list.push_back({
	        .id = "##foveation",
	        .label = _("Foveated encoding"),
	        .description = config.check_feature(feature::eye_gaze)
	                               ? _("Higher values focus image quality where you look at, improving latency, power efficiency and quality.")
	                               : _("Higher values focus image quality at the center, improving latency, power efficiency and quality."),
	        .ui = ui_kind::slider,
	        .get_int = [&config] { return int(std::lround((1 - config.get_stream_scale()) * 100)); },
	        .set_int = [&config](int v) {
		        if (not config.extended_config)
			        v = std::clamp(v, 30, 80);
		        config.set_stream_scale(1 - v * 0.01);
		        config.save(); },
	        .v_min = 0,
	        .v_max = 80,
	        .fmt = "%d%%",
	        .default_int = int(std::lround((1 - default_config.get_stream_scale()) * 100)),
	        .enabled = [&ctx] { return not ctx.in_game; },
	        .disabled_tooltip = disconnect_tip,
	});

	auto codec_name = [](std::optional<wivrn::video_codec> codec) -> std::string {
		if (not codec)
			return _C("Codec", "Automatic");
		switch (*codec)
		{
			case wivrn::h264:
				return _C("Codec", "H.264");
			case wivrn::h265:
				return _C("Codec", "HEVC (H.265)");
			case wivrn::av1:
				return _C("Codec", "AV1");
			case wivrn::raw:
				break;
		}
		return _C("Codec", "Automatic");
	};

	std::vector<wivrn::video_codec> codecs;
	for (auto c: wivrn::decoder::supported_codecs())
		if (c != wivrn::raw)
			codecs.push_back(c);

	list.push_back({
	        .id = "##codec",
	        .label = _C("setting name", "Video codec"),
	        .description = _("How video is compressed before it is sent to the headset."),
	        .ui = ui_kind::combo,
	        .get_int = [&config, codecs] { return config.codec ? index(codecs, *config.codec).value_or(-1) + 1 : 0; },
	        .set_int = [&config, codecs](int v) {
		        config.codec = v == 0 ? std::nullopt : std::optional(codecs[v - 1]);
		        config.save(); },
	        .options = [codecs, codec_name] {
		        std::vector<std::string> opts;
		        opts.push_back(codec_name(std::nullopt));
		        for (auto c: codecs)
			        opts.push_back(codec_name(c));
		        return opts; },
	        .title = _C("setting name", "Video codec"),
	        .default_int = default_config.codec ? index(codecs, *default_config.codec).value_or(-1) + 1 : 0,
	        .enabled = [&ctx] { return not ctx.in_game; },
	        .disabled_tooltip = disconnect_tip,
	});

	if (config.codec == wivrn::h265 or config.codec == wivrn::av1)
	{
		list.push_back({
		        .id = "##ten_bit",
		        .label = _C("setting name", "10-bit color"),
		        .description = _("Higher color precision, supported by HEVC and AV1."),
		        .ui = ui_kind::toggle,
		        .get_bool = [&config] { return config.bit_depth == 10; },
		        .set_bool = [&config](bool v) { config.bit_depth = v ? 10 : 8; config.save(); },
		        .default_bool = default_config.bit_depth == 10,
		        .enabled = [&ctx] { return not ctx.in_game; },
		        .disabled_tooltip = disconnect_tip,
		});
	}

	const int mb = 1'000'000;
	list.push_back({
	        .id = "##bitrate",
	        .label = _("Bitrate"),
	        .description = _("Video data rate sent to the headset."),
	        .ui = ui_kind::slider,
	        .get_int = [&config] { return int(config.bitrate_bps / mb); },
	        .set_int = [&ctx, &config](int v) { config.bitrate_bps = uint32_t(v) * mb; config.save(); if (ctx.on_streaming_changed) ctx.on_streaming_changed(); },
	        .v_min = 5,
	        .v_max = int(config.max_bitrate() / mb),
	        .fmt = "%d Mbit/s",
	        .default_int = int(default_config.bitrate_bps / mb),
	});

	// in-stream: steer where foveation focuses quality
	if (ctx.in_game)
	{
		list.push_back({
		        .id = "##fov_override",
		        .label = _C("setting name", "Foveation center override"),
		        .description = _("Manually set where image quality is focused, instead of the center or your gaze."),
		        .ui = ui_kind::toggle,
		        .get_bool = [&config] { return config.override_foveation_enable; },
		        .set_bool = [&ctx, &config](bool v) {
			        config.override_foveation_enable = v;
			        config.save();
			        if (ctx.on_foveation_override_changed)
				        ctx.on_foveation_override_changed(); },
		        .default_bool = default_config.override_foveation_enable,
		});
		list.push_back({
		        .id = "##fov_adjust",
		        .label = _C("setting name", "Foveation center"),
		        .description = fmt::format(_F("Height {:.1f} °, distance {:.2f} m"), -config.override_foveation_pitch * 180 / M_PI, config.override_foveation_distance),
		        .ui = ui_kind::button,
		        .button_label = _C("button label to change the foveation center", "Change"),
		        .on_click = [&ctx] { if (ctx.enter_foveation_adjust) ctx.enter_foveation_adjust(); },
		        .enabled = [&config] { return config.override_foveation_enable; },
		        .disabled_tooltip = _("Enable foveation center override to change this setting."),
		});
	}

	ui::page_header(_cS("page header title", "Streaming"), _cS("page header subtitle", "How video is encoded and sent to the headset."));
	render_settings(ctx, "##streaming", list);

	if (wivrn::ui::confirm_modal(
	            "confirm disable in stream gui",
	            _C("confirmation messagebox title", "Disable in-stream window"),
	            _C("confirmation message", "Do you really want to disable the in-stream window?\nYou will not be able to re-open it with the thumbsticks."),
	            _("Yes"),
	            _("No"),
	            true) == 1)
	{
		config.enable_stream_gui = false;
		config.save();
	}
}

void settings_post_processing(const settings_context & ctx)
{
	auto & config = ctx.config;
	auto & default_config = ctx.default_config;
	std::vector<setting> list;

	if (application::get_openxr_post_processing_supported())
	{
		auto flag_name = [](XrCompositionLayerSettingsFlagsFB f) -> std::string {
			switch (f)
			{
				case XR_COMPOSITION_LAYER_SETTINGS_NORMAL_SUPER_SAMPLING_BIT_FB:
				case XR_COMPOSITION_LAYER_SETTINGS_NORMAL_SHARPENING_BIT_FB:
					return _C("openxr_post_processing", "Normal");
				case XR_COMPOSITION_LAYER_SETTINGS_QUALITY_SUPER_SAMPLING_BIT_FB:
				case XR_COMPOSITION_LAYER_SETTINGS_QUALITY_SHARPENING_BIT_FB:
					return _C("openxr_post_processing", "Quality");
				default:
					return _C("openxr_post_processing", "Disabled");
			}
		};

		auto flag_combo = [&](const char * id, std::string label, std::string desc, std::array<XrCompositionLayerSettingsFlagsFB, 3> flags, XrCompositionLayerSettingsFlagsFB configuration::openxr_post_processing_settings::* member) {
			std::string title = label;
			list.push_back({
			        .id = id,
			        .label = std::move(label),
			        .description = std::move(desc),
			        .ui = ui_kind::combo,
			        .get_int = [&config, flags, member] { return index(flags, config.openxr_post_processing.*member).value_or(0); },
			        .set_int = [&config, flags, member](int v) {
				        config.openxr_post_processing.*member = flags[v];
				        config.save(); },
			        .options = [flags, flag_name] {
				        std::vector<std::string> o;
				        for (auto f: flags)
					        o.push_back(flag_name(f));
				        return o; },
			        .title = std::move(title),
			        .default_int = index(flags, config.openxr_post_processing.*member).value_or(0),
			});
		};

		flag_combo("##supersampling", _("Supersampling"), _("Reduce flicker for high contrast edges. Useful when the input resolution is high compared to the headset display."), {0, XR_COMPOSITION_LAYER_SETTINGS_NORMAL_SUPER_SAMPLING_BIT_FB, XR_COMPOSITION_LAYER_SETTINGS_QUALITY_SUPER_SAMPLING_BIT_FB}, &configuration::openxr_post_processing_settings::super_sampling);
		flag_combo("##sharpening", _("Sharpening"), _("Improve clarity of high contrast edges and counteract blur. Useful when the input resolution is low compared to the headset display."), {0, XR_COMPOSITION_LAYER_SETTINGS_NORMAL_SHARPENING_BIT_FB, XR_COMPOSITION_LAYER_SETTINGS_QUALITY_SHARPENING_BIT_FB}, &configuration::openxr_post_processing_settings::sharpening);
	}

	ui::page_header(_S("Post-processing"), _cS("page header subtitle", "OpenXR layer supersampling and sharpening."));
	render_settings(ctx, "##post_processing", list);
}

void settings_audio(const settings_context & ctx)
{
	auto & config = ctx.config;
	auto & default_config = ctx.default_config;
	const std::string disconnect_tip = ctx.in_game ? _C("tooltip for disabled settings", "Disconnect to change this setting.") : std::string{};
	std::vector<setting> list;

	list.push_back({
	        .id = "##microphone",
	        .label = _C("setting name", "Microphone"),
	        .description = _("Stream the headset microphone to the PC."),
	        .ui = ui_kind::toggle,
	        .get_bool = [&config] { return config.check_feature(feature::microphone); },
	        .set_bool = [&config](bool v) { config.set_feature(feature::microphone, v); },
	        .default_bool = default_config.check_feature(feature::microphone),
	        .enabled = [&ctx] { return not ctx.in_game; },
	        .disabled_tooltip = disconnect_tip,
	});

	list.push_back({
	        .id = "##unprocessed",
	        .label = _C("setting name", "Unprocessed audio"),
	        .description = _("Disable audio filters, such as noise cancellation."),
	        .ui = ui_kind::toggle,
	        .get_bool = [&config] { return config.mic_unprocessed_audio; },
	        .set_bool = [&config](bool v) { config.mic_unprocessed_audio = v; config.save(); },
	        .default_bool = default_config.mic_unprocessed_audio,
	        .enabled = [&ctx, &config] { return not ctx.in_game and config.check_feature(feature::microphone); },
	        .disabled_tooltip = disconnect_tip,
	});

	ui::page_header(_cS("page header title", "Audio"), _cS("page header subtitle", "Microphone streamed to the PC."));
	render_settings(ctx, "##audio", list);
}

void settings_devices(const settings_context & ctx)
{
	auto & config = application::get_config();
	auto & default_config = ctx.default_config;
	std::vector<setting> list;

	list.push_back({
	        .id = "##keyboard",
	        .label = _C("setting name", "Keyboard"),
	        .description = _("Forward the keyboard from the headset to the PC."),
	        .ui = ui_kind::toggle,
	        .get_bool = [&config] { return config.forward_keyboard; },
	        .set_bool = [&config](bool v) { config.forward_keyboard = v; config.save(); },
	        .default_bool = default_config.forward_keyboard,
	});

	list.push_back({
	        .id = "##mouse",
	        .label = _C("setting name", "Mouse"),
	        .description = _("Forward the mouse from the headset to the PC."),
	        .ui = ui_kind::toggle,
	        .get_bool = [&config] { return config.forward_mouse; },
	        .set_bool = [&config](bool v) { config.forward_mouse = v; config.save(); },
	        .default_bool = default_config.forward_mouse,
	});

	list.push_back({
	        .id = "##gamepad",
	        .label = _C("setting name", "Gamepad"),
	        .description = _("Forward the gamepad from the headset to the PC."),
	        .ui = ui_kind::toggle,
	        .get_bool = [&config] { return config.forward_gamepad; },
	        .set_bool = [&config](bool v) { config.forward_gamepad = v; config.save(); },
	        .default_bool = default_config.forward_gamepad,
	});

	ui::page_header(_cS("page header title", "Devices"), _cS("page header subtitle", "Forward input devices to the PC."));
	render_settings(ctx, "##devices", list);

	if (ctx.server_hid_forwarding == false and (config.forward_keyboard or config.forward_mouse or config.forward_gamepad))
		ui::chip(_("The server does not allow forwarded input devices"), ui::chip_style::warning);
}

bool settings_tracking(const settings_context & ctx)
{
	auto & config = ctx.config;
	auto & default_config = ctx.default_config;
	const std::string disconnect_tip = ctx.in_game ? _C("tooltip for disabled settings", "Disconnect to change this setting.") : std::string{};
	std::vector<setting> list;
	bool changed = false;
	const bool body_settings_applyable = not ctx.in_game || bool(ctx.on_body_tracking_changed);
	const std::string body_settings_tip = ctx.in_game && not ctx.on_body_tracking_changed
	                                               ? _("This setting cannot be applied while streaming.")
	                                               : disconnect_tip;
	auto body_setting_enabled = [&ctx] { return not ctx.in_game || bool(ctx.on_body_tracking_changed); };

	auto feature_toggle = [&](const char * id, std::string label, std::string desc, feature f) {
		list.push_back({
		        .id = id,
		        .label = std::move(label),
		        .description = std::move(desc),
		        .ui = ui_kind::toggle,
		        .get_bool = [&config, f] { return config.check_feature(f); },
		        .set_bool = [&config, f](bool v) { config.set_feature(f, v); },
		        .default_bool = default_config.check_feature(f),
		        .enabled = [&ctx] { return not ctx.in_game; },
		        .disabled_tooltip = disconnect_tip,
		});
	};

	if (ctx.system.hand_tracking_supported())
		feature_toggle("##hand", _C("setting name", "Hand tracking"), _("Track your hands for input when controllers are down."), feature::hand_tracking);

	if (application::get_eye_gaze_supported())
		feature_toggle("##eye", _C("setting name", "Eye tracking"), _("Used by foveated encoding to focus quality where you look."), feature::eye_gaze);

	if (ctx.system.face_tracker_supported() != xr::face_tracker_type::none)
		feature_toggle("##face", _C("setting name", "Face tracking"), _("Stream facial expressions to the PC."), feature::face_tracking);

	const auto body_tracker = ctx.system.body_tracker_supported();
	const bool body_fidelity_supported = ctx.system.meta_body_tracking_fidelity_supported();
	const bool body_height_override_supported = ctx.system.meta_body_tracking_height_override_supported();
	const bool body_calibration_reset_supported =
	        ctx.instance.has_extension(XR_META_BODY_TRACKING_CALIBRATION_EXTENSION_NAME);

	std::vector<char> body_parts_default;
	std::vector<std::string> body_parts_names;
	std::vector<from_headset::body_part_mask> body_parts_bit;
	for (const auto & [bit, name]: {
	             std::make_pair(from_headset::body_part_mask::chest, _C("virtual body tracker selection", "Chest")),
	             std::make_pair(from_headset::body_part_mask::left_elbow, _C("virtual body tracker selection", "Left elbow")),
	             std::make_pair(from_headset::body_part_mask::right_elbow, _C("virtual body tracker selection", "Right elbow")),
	             std::make_pair(from_headset::body_part_mask::hip, _C("virtual body tracker selection", "Hip")),
	             std::make_pair(from_headset::body_part_mask::left_knee, _C("virtual body tracker selection", "Left knee")),
	             std::make_pair(from_headset::body_part_mask::right_knee, _C("virtual body tracker selection", "Right knee")),
	             std::make_pair(from_headset::body_part_mask::left_foot, _C("virtual body tracker selection", "Left foot")),
	             std::make_pair(from_headset::body_part_mask::right_foot, _C("virtual body tracker selection", "Right foot")),
	     })
	{
		if (body_tracker == xr::body_tracker_type::fb and bit > from_headset::body_part_mask::hip)
			break;
		body_parts_default.push_back((default_config.body_part_mask & std::to_underlying(bit)) != 0);
		body_parts_names.push_back(name);
		body_parts_bit.push_back(bit);
	}
	const auto supported_body_parts = [&body_parts_bit] {
		std::underlying_type_t<from_headset::body_part_mask> mask = 0;
		for (const auto bit: body_parts_bit)
			mask |= std::to_underlying(bit);
		return mask;
	}();

	if (body_tracker != xr::body_tracker_type::none)
	{
		list.push_back({
		        .id = "##body",
		        .label = _C("setting name", "Body tracking"),
		        .description = body_tracker == xr::body_tracker_type::fb or body_tracker == xr::body_tracker_type::meta
		                               ? _("Requires 'Hand and body tracking' to be enabled in the Quest movement tracking settings, otherwise body data will be guessed from controller and headset positions.")
		                               : _("Stream body joint positions to the PC."),
		        .ui = ui_kind::toggle,
		        .get_bool = [&config] { return config.check_feature(feature::body_tracking); },
		        .set_bool = [&config, &changed, supported_body_parts](bool v) {
			        // A zero mask cannot produce any virtual trackers.  Seed it from the
			        // parts supported by this runtime before set_feature() persists the
			        // updated feature flag.
			        if (v && config.body_part_mask == 0 && supported_body_parts != 0)
			        {
			                config.body_part_mask = supported_body_parts;
			                changed = true;
			        }
			        config.set_feature(feature::body_tracking, v);
		        },
				.default_bool = default_config.check_feature(feature::body_tracking),
				.enabled = [&ctx] { return not ctx.in_game; },
				.disabled_tooltip = disconnect_tip,
		});

		list.push_back({
		        .id = "##body_tracking_parts",
		        .label = _C("setting name", "Virtual body trackers"),
		        .description = _("Create virtual tracker devices."),
		        .ui = ui_kind::combo_multi,
				.get_multi = [&config, &body_parts_bit](int index) {
			        const auto underlying = std::to_underlying(body_parts_bit[index]);
			        return config.body_part_mask & underlying; },
		        .set_multi = [&config, &body_parts_bit, &changed](int index, bool value) {
			        const auto underlying = std::to_underlying(body_parts_bit[index]);
				const bool current = config.body_part_mask & underlying;
				if (current == value)
					return;
				if (value)
					config.body_part_mask |= underlying;
				else
					config.body_part_mask &= ~underlying;
				changed = true; },
				.options = [&body_parts_names]() { return body_parts_names; },
				.title = _C("setting name", "Virtual body trackers"),
				.default_multi = body_parts_default,
				.enabled = body_setting_enabled,
				.disabled_tooltip = body_settings_tip,
		});

		static constexpr std::array profiles{
		        wivrn::body_tracking_profile::stable,
		        wivrn::body_tracking_profile::balanced,
		        wivrn::body_tracking_profile::responsive,
		        wivrn::body_tracking_profile::custom,
		};
		list.push_back({
		        .id = "##body_tracking_profile",
		        .label = _C("body tracking setting", "Profile"),
		        .description = _("Choose a stability/latency preset, or Custom to tune the tracking filters below."),
		        .ui = ui_kind::segmented,
		        .get_int = [&config] { return index(profiles, config.body_tracking.profile).value_or(1); },
		        .set_int = [&config, &changed](int value) {
			        config.body_tracking.profile = profiles[value];
			        changed = true; },
		        .options = [] { return std::vector<std::string>{_("Stable"), _("Balanced"), _("Responsive"), _("Custom")}; },
		        .default_int = index(profiles, default_config.body_tracking.profile).value_or(1),
		        .enabled = body_setting_enabled,
		        .disabled_tooltip = body_settings_tip,
		});

		static constexpr std::array fidelities{
		        wivrn::body_tracking_fidelity::automatic,
		        wivrn::body_tracking_fidelity::low,
		        wivrn::body_tracking_fidelity::high,
		};
		list.push_back({
		        .id = "##body_tracking_fidelity",
		        .label = _C("body tracking setting", "Fidelity"),
		        .description = _("Automatic uses WiVRn's default (High when Meta fidelity is supported)."),
		        .ui = ui_kind::segmented,
		        .get_int = [&config] { return index(fidelities, config.body_tracking.fidelity).value_or(0); },
		        .set_int = [&config, &changed](int value) {
			        config.body_tracking.fidelity = fidelities[value];
			        changed = true; },
		        .options = [] { return std::vector<std::string>{_C("body tracking fidelity", "Auto"), _("Low"), _("High")}; },
		        .default_int = index(fidelities, default_config.body_tracking.fidelity).value_or(0),
		        .enabled = [body_setting_enabled, body_fidelity_supported] {
			        return body_setting_enabled() && body_fidelity_supported;
		        },
		        .disabled_tooltip = body_fidelity_supported
		                                    ? body_settings_tip
		                                    : _("Body tracking fidelity is not supported by this runtime."),
		});

		static constexpr std::array calibration_modes{
		        wivrn::body_tracking_calibration_mode::automatic,
		        wivrn::body_tracking_calibration_mode::manual,
		};
		list.push_back({
		        .id = "##body_tracking_calibration",
		        .label = _C("body tracking setting", "Calibration"),
		        .description = _("Automatic calibration follows the headset. Manual calibration uses the height entered below."),
		        .ui = ui_kind::segmented,
		        .get_int = [&config] { return index(calibration_modes, config.body_tracking.calibration_mode).value_or(0); },
		        .set_int = [&config, &default_config, &changed](int value) {
			        config.body_tracking.calibration_mode = calibration_modes[value];
			        if (config.body_tracking.calibration_mode == wivrn::body_tracking_calibration_mode::manual &&
			            !config.body_tracking.body_height_mm)
				        config.body_tracking.body_height_mm = default_config.body_tracking.body_height_mm.value_or(1700);
			        changed = true; },
		        .options = [] { return std::vector<std::string>{_C("body tracking calibration", "Auto"), _("Manual")}; },
		        .default_int = index(calibration_modes, default_config.body_tracking.calibration_mode).value_or(0),
		        .enabled = [body_setting_enabled, body_height_override_supported] {
			        return body_setting_enabled() && body_height_override_supported;
		        },
		        .disabled_tooltip = body_height_override_supported
		                                    ? body_settings_tip
		                                    : _("Body height override is not supported by this runtime."),
		});

		list.push_back({
		        .id = "##body_tracking_height",
		        .label = _C("body tracking setting", "Body height"),
		        .description = _("Height used for manual calibration."),
		        .ui = ui_kind::slider,
		        .get_int = [&config, &default_config] {
			        return int(config.body_tracking.body_height_mm.value_or(default_config.body_tracking.body_height_mm.value_or(1700)));
		        },
		        .set_int = [&config, &changed](int value) {
			        config.body_tracking.body_height_mm = uint16_t(std::clamp(value, 1000, 2500));
			        changed = true; },
		        .v_min = 1000,
		        .v_max = 2500,
		        .fmt = "%d mm",
		        .default_int = int(default_config.body_tracking.body_height_mm.value_or(1700)),
		        .enabled = [&ctx, body_setting_enabled, body_height_override_supported] {
			        return body_setting_enabled() && body_height_override_supported &&
			               ctx.config.body_tracking.calibration_mode == wivrn::body_tracking_calibration_mode::manual;
		        },
		        .disabled_tooltip = body_height_override_supported
		                                    ? body_settings_tip
		                                    : _("Body height override is not supported by this runtime."),
		});

		static constexpr std::array supervisors{
		        wivrn::body_tracking_supervisor::off,
		        wivrn::body_tracking_supervisor::automatic,
		        wivrn::body_tracking_supervisor::server,
		};
		list.push_back({
		        .id = "##body_tracking_supervisor",
		        .label = _C("body tracking setting", "Supervisor"),
		        .description = _("Select whether body samples are supervised before they are published."),
		        .ui = ui_kind::segmented,
		        .get_int = [&config] { return index(supervisors, config.body_tracking.supervisor).value_or(0); },
		        .set_int = [&config, &changed](int value) {
			        config.body_tracking.supervisor = supervisors[value];
			        changed = true; },
		        .options = [] { return std::vector<std::string>{_("Off"), _C("body tracking supervisor", "Auto"), _("Server")}; },
		        .default_int = index(supervisors, default_config.body_tracking.supervisor).value_or(0),
		        .enabled = body_setting_enabled,
		        .disabled_tooltip = body_settings_tip,
		});

		static constexpr std::array supervisor_paths{
		        wivrn::body_tracking_supervisor_path::filtered,
		        wivrn::body_tracking_supervisor_path::raw,
		};
		list.push_back({
		        .id = "##body_tracking_supervisor_path",
		        .label = _C("body tracking setting", "Supervisor path"),
		        .description = _("Supervised uses the filtered relation; Raw forwards the unfiltered relation."),
		        .ui = ui_kind::segmented,
		        .get_int = [&config] { return index(supervisor_paths, config.body_tracking.supervisor_path).value_or(0); },
		        .set_int = [&config, &changed](int value) {
			        config.body_tracking.supervisor_path = supervisor_paths[value];
			        changed = true; },
		        .options = [] { return std::vector<std::string>{_("Supervised"), _("Raw")}; },
		        .default_int = index(supervisor_paths, default_config.body_tracking.supervisor_path).value_or(0),
		        .enabled = body_setting_enabled,
		        .disabled_tooltip = body_settings_tip,
		});

		const bool custom_profile = config.body_tracking.profile == wivrn::body_tracking_profile::custom;
		const bool supervisor_enabled = config.body_tracking.supervisor != wivrn::body_tracking_supervisor::off;
		const bool custom_enabled = body_settings_applyable && custom_profile && supervisor_enabled;
		const std::string custom_tip = not custom_profile
		                                       ? _("Select the Custom profile to tune this setting.")
		                               : not supervisor_enabled
		                                       ? _("Enable the body tracking supervisor to tune this setting.")
		                                       : body_settings_tip;
		list.push_back({
		        .id = "##body_tracking_smoothing_position",
		        .label = _C("body tracking advanced setting", "Position smoothing"),
		        .description = _("Smooth position changes to reduce jitter; higher values add latency."),
		        .ui = ui_kind::slider,
		        .get_int = [&config] { return int(config.body_tracking.smoothing_position_permille / 10); },
		        .set_int = [&config, &changed](int value) {
			        config.body_tracking.smoothing_position_permille = uint16_t(std::clamp(value, 0, 100) * 10);
			        changed = true; },
		        .v_min = 0,
		        .v_max = 100,
		        .fmt = "%d%%",
		        .default_int = default_config.body_tracking.smoothing_position_permille / 10,
		        .enabled = [custom_enabled] { return custom_enabled; },
		        .disabled_tooltip = custom_tip,
		});

		list.push_back({
		        .id = "##body_tracking_smoothing_rotation",
		        .label = _C("body tracking advanced setting", "Rotation smoothing"),
		        .description = _("Smooth rotation changes to reduce jitter; higher values add latency."),
		        .ui = ui_kind::slider,
		        .get_int = [&config] { return int(config.body_tracking.smoothing_rotation_permille / 10); },
		        .set_int = [&config, &changed](int value) {
			        config.body_tracking.smoothing_rotation_permille = uint16_t(std::clamp(value, 0, 100) * 10);
			        changed = true; },
		        .v_min = 0,
		        .v_max = 100,
		        .fmt = "%d%%",
		        .default_int = default_config.body_tracking.smoothing_rotation_permille / 10,
		        .enabled = [custom_enabled] { return custom_enabled; },
		        .disabled_tooltip = custom_tip,
		});

		list.push_back({
		        .id = "##body_tracking_confidence",
		        .label = _C("body tracking advanced setting", "Confidence threshold"),
		        .description = _("Reject body poses below this confidence level."),
		        .ui = ui_kind::slider,
		        .get_int = [&config] { return int(config.body_tracking.confidence_threshold_permille / 10); },
		        .set_int = [&config, &changed](int value) {
			        config.body_tracking.confidence_threshold_permille = uint16_t(std::clamp(value, 0, 100) * 10);
			        changed = true; },
		        .v_min = 0,
		        .v_max = 100,
		        .fmt = "%d%%",
		        .default_int = default_config.body_tracking.confidence_threshold_permille / 10,
		        .enabled = [custom_enabled] { return custom_enabled; },
		        .disabled_tooltip = custom_tip,
		});

		list.push_back({
		        .id = "##body_tracking_dropout_hold",
		        .label = _C("body tracking advanced setting", "Dropout hold"),
		        .description = _("Hold the last valid pose for this long during a brief dropout."),
		        .ui = ui_kind::slider,
		        .get_int = [&config] { return int(config.body_tracking.dropout_hold_ms); },
		        .set_int = [&config, &changed](int value) {
			        config.body_tracking.dropout_hold_ms = uint16_t(std::clamp(value, 0, 2000));
			        changed = true; },
		        .v_min = 0,
		        .v_max = 2000,
		        .fmt = "%d ms",
		        .default_int = default_config.body_tracking.dropout_hold_ms,
		        .enabled = [custom_enabled] { return custom_enabled; },
		        .disabled_tooltip = custom_tip,
		});

		list.push_back({
		        .id = "##body_tracking_recovery",
		        .label = _C("body tracking advanced setting", "Recovery samples"),
		        .description = _("Require consecutive valid samples before declaring tracking recovered."),
		        .ui = ui_kind::slider,
		        .get_int = [&config] { return int(config.body_tracking.recovery_samples); },
		        .set_int = [&config, &changed](int value) {
			        config.body_tracking.recovery_samples = uint16_t(std::clamp(value, 1, 32));
			        changed = true; },
		        .v_min = 1,
		        .v_max = 32,
		        .fmt = "%d",
		        .default_int = default_config.body_tracking.recovery_samples,
		        .enabled = [custom_enabled] { return custom_enabled; },
		        .disabled_tooltip = custom_tip,
		});

		list.push_back({
		        .id = "##body_tracking_prediction",
		        .label = _C("body tracking advanced setting", "Prediction"),
		        .description = _("Predict body motion ahead by this many milliseconds."),
		        .ui = ui_kind::slider,
		        .get_int = [&config] { return int(config.body_tracking.prediction_ms); },
		        .set_int = [&config, &changed](int value) {
			        config.body_tracking.prediction_ms = uint16_t(std::clamp(value, 0, 100));
			        changed = true; },
		        .v_min = 0,
		        .v_max = 100,
		        .fmt = "%d ms",
		        .default_int = default_config.body_tracking.prediction_ms,
		        .enabled = [custom_enabled] { return custom_enabled; },
		        .disabled_tooltip = custom_tip,
		});

		list.push_back({
		        .id = "##body_tracking_floor_assist",
		        .label = _C("body tracking advanced setting", "Floor assist"),
		        .description = _("Keep feet aligned to the floor when the provider does not report a stable floor."),
		        .ui = ui_kind::toggle,
		        .get_bool = [&config] { return config.body_tracking.floor_assist; },
		        .set_bool = [&config, &changed](bool value) {
			        config.body_tracking.floor_assist = value;
			        changed = true; },
		        .default_bool = default_config.body_tracking.floor_assist,
		        .enabled = [custom_enabled] { return custom_enabled; },
		        .disabled_tooltip = custom_tip,
		});

		if (ctx.in_game)
		{
			for (const auto & [scope, id, label, description]: {
			             std::tuple{wivrn::body_tracking_reset::tracking, "##body_tracking_reset_tracking", _C("body tracking reset", "Reset tracking"), _("Clear the current tracking state and start tracking again.")},
			             std::tuple{wivrn::body_tracking_reset::calibration, "##body_tracking_reset_calibration", _C("body tracking reset", "Reset calibration"), _("Clear the current calibration and request a new one.")},
			             std::tuple{wivrn::body_tracking_reset::all, "##body_tracking_reset_all", _C("body tracking reset", "Reset all"), _("Clear tracking and calibration state.")},
		             })
			{
				list.push_back({
				        .id = id,
				        .label = label,
				        .description = description,
				        .ui = ui_kind::button,
				        .button_label = label,
				        .on_click = [&ctx, scope] {
					        if (ctx.on_body_tracking_reset)
					                ctx.on_body_tracking_reset(scope);
				        },
				        .enabled = [&ctx, scope, body_calibration_reset_supported] {
					        const bool calibration_supported = scope != wivrn::body_tracking_reset::calibration ||
					                                           body_calibration_reset_supported;
					        return bool(ctx.on_body_tracking_reset) &&
					               ctx.config.check_feature(feature::body_tracking) && calibration_supported;
				        },
				        .disabled_tooltip = _("Body tracking reset is unavailable for this stream."),
				});
			}
		}
	}

	ui::page_header(_cS("page header title", "Tracking"), _cS("page header subtitle", "Body and input tracking sent to the PC."));
	render_settings(ctx, "##tracking", list);
	if (changed)
	{
		config.save();
		if (ctx.on_body_tracking_changed)
			ctx.on_body_tracking_changed(config.body_tracking);
	}
	if (config.check_feature(feature::body_tracking) && config.body_part_mask == 0)
		ui::chip(_("Body tracking is enabled, but no body parts are selected."), ui::chip_style::warning);

	return changed;
}

void settings_system(const settings_context & ctx)
{
	auto & config = ctx.config;
	auto & default_config = ctx.default_config;
	std::vector<setting> list;

	auto language_name = [](const std::locale & loc = std::locale()) {
		return boost::locale::pgettext("language selection, replace with the name of the language", "English", loc);
	};
	std::vector<std::tuple<std::string, std::locale, std::string>> languages;
	for (const auto & msg_info: get_locales())
	{
		std::string code = msg_info.language;
		if (not msg_info.country.empty())
			code += "_" + msg_info.country;
		std::locale loc(std::locale(), boost::locale::gnu_gettext::create_messages_facet<char>(msg_info));
		languages.emplace_back(language_name(loc), loc, std::move(code));
	}
	std::ranges::sort(languages, [](auto & l, auto & r) { return std::get<0>(l) < std::get<0>(r); });

	auto language_index = [languages](const configuration & cfg) {
		if (cfg.locale != "")
			for (size_t i = 0; i < languages.size(); ++i)
				if (std::get<2>(languages[i]) == cfg.locale)
					return int(i) + 1;
		return 0;
	};

	list.push_back({
	        .id = "##language",
	        .label = _("Language"),
	        .description = _C("setting name", "Interface language."),
	        .ui = ui_kind::combo,
	        .get_int = [&config, language_index] { return language_index(config); },
	        .set_int = [&config, languages](int v) {
		        config.locale = v == 0 ? "" : std::get<2>(languages[v - 1]);
		        config.save();
		        application::instance().load_locale(); },
	        .options = [languages] {
		        std::vector<std::string> opts;
		        opts.push_back(_C("item in the interface language combobox", "System language"));
		        for (const auto & [lang, loc, code]: languages)
			        opts.push_back(lang);
		        return opts; },
	        .title = _("Language"),
	        .default_int = language_index(default_config),
	});

	list.push_back({
	        .id = "##extended",
	        .label = _C("setting name", "Extended configuration"),
	        .description = _("Allows unsafe configuration values, use at your own risk."),
	        .ui = ui_kind::toggle,
	        .get_bool = [&config] { return config.extended_config; },
	        .set_bool = [&config](bool v) { config.extended_config = v; config.save(); },
	        .default_bool = default_config.extended_config,
	});

	if (ctx.instance.has_extension(XR_EXT_PERFORMANCE_SETTINGS_EXTENSION_NAME))
	{
		list.push_back({
		        .id = "##high_power",
		        .label = _("High power mode"),
		        .description = _("Increase power usage to allow higher resolution and refresh rate. Drains battery and runs hot."),
		        .ui = ui_kind::toggle,
		        .get_bool = [&config] { return config.high_power_mode; },
		        .set_bool = [&config](bool v) { config.high_power_mode = v; config.save(); },
		        .default_bool = default_config.high_power_mode,
		});
	}

	bool disable_instream_gui = false;
	bool disable_instream_gui_popup_open = ImGui::IsPopupOpen("confirm disable in stream gui");
	list.push_back({
	        .id = "##stream_gui",
	        .label = _C("setting name", "In-stream window"),
	        .description = _("Enables the configuration window to be shown while the game is streaming. If enabled, the window is activated by pressing both thumbsticks."),
	        .ui = ui_kind::toggle,
	        .get_bool = [&config, &disable_instream_gui_popup_open] { return config.enable_stream_gui and not disable_instream_gui_popup_open; },
	        .set_bool = [&ctx, &config, &disable_instream_gui](bool v) {
			if (ctx.in_game and not v)
			{
				disable_instream_gui = true;
			}
			else
			{
				config.enable_stream_gui = v;
				config.save();
			} },
	        .default_bool = default_config.enable_stream_gui,
	});

	ui::page_header(_cS("page header title", "System"), _cS("page header suibtitle", "Language and advanced options."));
	render_settings(ctx, "##system", list);

	if (disable_instream_gui)
		ImGui::OpenPopup("confirm disable in stream gui");
}

void settings_theme(const settings_context & ctx)
{
	ui::theme & theme = ui::current();
	auto & config = ctx.config;
	auto & default_config = ctx.default_config;

	ui::page_header(_cS("page header title", "Theme"), _cS("page header subtitle", "Accent color, palette and sizing of the interface."));

	ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ui::metrics::card_item_spacing);
	const float control_w = ui::metrics::setting_control_width;

	ui::begin_card("##theme");
	{
		// Accent
		ImGui::TextUnformatted(_cS("setting name", "Accent color"));
		ImGui::Dummy({0, 2});
		for (const auto & swatch: ui::accent_swatches())
		{
			const bool selected = theme.accent.x == swatch.base.x and theme.accent.y == swatch.base.y and theme.accent.z == swatch.base.z;
			const auto flags = ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoBorder | (selected ? 0 : ImGuiColorEditFlags_NoDragDrop);
			if (ImGui::ColorButton(swatch.name, swatch.base, flags, {ImGui::GetFrameHeight() * 1.4f, ImGui::GetFrameHeight() * 1.4f}))
			{
				ui::set_accent(swatch);
				config.theme_accent = swatch.name;
				config.save();
			}
			ImGui::SameLine();
		}
		ImGui::NewLine();

		ui::row_separator();

		// Preset
		const auto preset_list = ui::presets();
		std::vector<ui::combo_item> preset_items;
		for (const auto & p: preset_list)
			preset_items.push_back({p.localized_name.c_str()});

		// seed the selection from the saved preset so the box matches what is applied
		auto f = [&](const configuration & cfg) {
			for (int i = 0; i < int(preset_list.size()); ++i)
				if (preset_list[i].name == cfg.theme_preset)
					return i;
			return 1; // dark_default
		};
		static int preset = f(config);
		static const int preset_default = f(default_config);

		ui::setting_label(_cS("setting name", "Preset"), _cS("setting description", "Surface and background palette"), control_w);
		if (ui::combo("##preset", _("Theme preset"), preset_items, &preset, control_w, &preset_default))
		{
			// keep the accent across a preset change, only the surfaces swap
			const ImVec4 keep_accent = theme.accent;
			const ImVec4 keep_hover = theme.accent_hovered;
			const ImVec4 keep_active = theme.accent_active;
			ui::set_theme(preset_list[preset]);
			theme.accent = keep_accent;
			theme.accent_hovered = keep_hover;
			theme.accent_active = keep_active;
			config.theme_preset = preset_list[preset].name;
			config.save();
		}

		ui::row_separator();

		// Rounding
		static const int rounding_default = default_config.theme_rounding;
		int rounding = int(theme.rounding);
		ui::setting_label(_cS("setting label", "Rounding"), _cS("setting description", "Corner radius of controls"), control_w);
		if (ui::slider_int("##rounding", &rounding, 0, 20, "%d px", {control_w, 0}, &rounding_default))
		{
			theme.rounding = float(rounding);
			config.theme_rounding = theme.rounding;
			config.save();
		}

		ui::row_separator();

		static const int card_rounding_default = 14;
		int card_rounding = int(theme.card_rounding);
		ui::setting_label(_cS("setting label", "Card rounding"), _cS("setting description", "Corner radius of panels"), control_w);
		if (ui::slider_int("##card_rounding", &card_rounding, 0, 28, "%d px", {control_w, 0}, &card_rounding_default))
		{
			theme.card_rounding = float(card_rounding);
			config.theme_card_rounding = theme.card_rounding;
			config.save();
		}

		ui::row_separator();

		// global font scale, 100% is the design default
		static const int font_scale_default = 100;
		int font_scale = int(theme.font_scale * 100);
		ui::setting_label(_cS("setting name", "Text size"), _cS("setting description", "Global font scale"), control_w);
		if (ui::slider_int("##font_scale", &font_scale, 60, 140, "%d%%", {control_w, 0}, &font_scale_default))
		{
			theme.font_scale = float(font_scale) / 100.f;
			config.theme_font_scale = theme.font_scale;
			config.save();
		}

		ui::row_separator();

		// Panel transparency, independent of the selected preset
		static const int opacity_default = int(default_config.theme_background_alpha * 100 + 0.5);
		int opacity = int(ui::background_alpha() * 100 + 0.5);
		ui::setting_label(_cS("setting name", "Panel opacity"), _cS("setting description", "Opacity of the panel and card backgrounds"), control_w);
		if (ui::slider_int("##opacity", &opacity, 20, 100, "%d%%", {control_w, 0}, &opacity_default))
		{
			ui::background_alpha() = float(opacity) / 100.f;
			config.theme_background_alpha = ui::background_alpha();
			config.save();
		}

		ui::end_card();
	}

	ImGui::PopStyleVar(); // ImGuiStyleVar_ItemSpacing
}

} // namespace wivrn::gui
