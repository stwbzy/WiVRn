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

#define IMGUI_DEFINE_MATH_OPERATORS

#include "ui_widgets.h"

#include "imgui.h"
#include "imgui_internal.h"
#include "ui_theme.h"

#include "IconsFontAwesome6.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <unordered_map>
#include <vector>

// drag-to-scroll helper, defined in imgui_impl.cpp
void ScrollWhenDragging();

namespace wivrn::ui
{

namespace
{
std::function<void()> hover_haptic_hook;
std::function<void(const char *)> tooltip_hook;

// Small frame-rate independent transitions shared by the touch widgets. Values are
// keyed by ImGui IDs, so switching pages naturally starts a fresh reveal.
std::unordered_map<ImGuiID, float> animation_values;

float animate_scalar(ImGuiID id, float target, float speed = 14.f)
{
	float & value = animation_values[id];
	const float dt = ImClamp(ImGui::GetIO().DeltaTime, 0.001f, 0.05f);
	value += (target - value) * (1.f - std::exp(-speed * dt));
	if (std::abs(target - value) < 0.001f)
		value = target;
	return value;
}

ImVec4 mix(const ImVec4 & a, const ImVec4 & b, float amount)
{
	amount = ImClamp(amount, 0.f, 1.f);
	return {a.x + (b.x - a.x) * amount,
	        a.y + (b.y - a.y) * amount,
	        a.z + (b.z - a.z) * amount,
	        a.w + (b.w - a.w) * amount};
}

// height reserved for the pinned sidebar footer, set by begin_sidebar
float sidebar_footer_height = 0;
} // namespace

void set_hover_haptic(std::function<void()> hook)
{
	hover_haptic_hook = std::move(hook);
}

void set_tooltip_hook(std::function<void(const char *)> hook)
{
	tooltip_hook = std::move(hook);
}

// fire the hover haptic for the last submitted item, hook checks if it is hovered
static void hover_haptic()
{
	if (hover_haptic_hook)
		hover_haptic_hook();
}

// show a tooltip for the last item, caller ensures it is hovered
static void show_tooltip(const std::string & text)
{
	if (not text.empty() and tooltip_hook)
		tooltip_hook(text.c_str());
}

void page_header(const std::string & title, const std::string & subtitle)
{
	const theme & t = current();
	const ImGuiID header_id = ImGui::GetID(("##page_header_" + title).c_str());
	const float reveal = animate_scalar(header_id, 1.f, 9.f);
	ImGui::PushStyleVar(ImGuiStyleVar_Alpha, 0.18f + 0.82f * reveal);

	ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * metrics::font_title);
	ImGui::TextUnformatted(title.c_str());
	ImGui::PopFont();

	if (not subtitle.empty())
	{
		ImGui::PushStyleColor(ImGuiCol_Text, t.text_muted);
		ImGui::TextUnformatted(subtitle.c_str());
		ImGui::PopStyleColor();
	}
	ImGui::PopStyleVar();

	// A short animated neon rule gives each page a stable visual anchor without
	// changing its layout or adding another control.
	const ImVec2 line = ImGui::GetCursorScreenPos();
	ImVec4 line_color = t.accent;
	line_color.w = 0.56f + 0.14f * std::sin(float(ImGui::GetTime()) * 2.2f);
	ImGui::GetWindowDrawList()->AddRectFilled(line, line + ImVec2{64.f + 18.f * reveal, 3.f}, t.col(line_color), 1.5f);
	ImGui::Dummy({0, ImGui::GetStyle().ItemSpacing.y});
}

// a rounded panel, not a child window, so the page stays one scroll surface, drawn behind
// the content using last frame's height, pair with end_card
namespace
{
struct card_frame
{
	ImGuiID id;
	ImVec2 origin;
	float width;
	float content_max_x_backup;
	float work_max_x_backup;
	ImVec2 padding;
};
std::vector<card_frame> card_stack;
std::unordered_map<ImGuiID, float> card_heights;

bool begin_card_impl(const char * id, ImVec2 padding)
{
	ImGuiWindow * window = ImGui::GetCurrentWindow();
	const theme & t = current();
	const ImGuiID gid = window->GetID(id);

	card_stack.push_back({gid,
	                      window->DC.CursorPos,
	                      ImGui::GetContentRegionAvail().x,
	                      window->ContentRegionRect.Max.x,
	                      window->WorkRect.Max.x,
	                      padding});
	const card_frame & cf = card_stack.back();

	if (const float h = card_heights[gid]; h > 0)
	{
		const ImVec2 bb_max = {cf.origin.x + cf.width, cf.origin.y + h};
		const float reveal = animate_scalar(gid, 1.f, 8.f);
		ImVec4 shadow = {0, 0, 0, 0.28f * reveal};
		window->DrawList->AddRectFilled(cf.origin + ImVec2{0, 5}, bb_max + ImVec2{0, 5}, t.col(shadow), t.card_rounding + 2);
		ImVec4 card = t.card;
		card.w *= background_alpha() * (0.88f + 0.12f * reveal); // card fill follows the panel opacity
		window->DrawList->AddRectFilled(cf.origin, bb_max, t.col(card), t.card_rounding);
		if (t.border_size > 0)
			window->DrawList->AddRect(cf.origin, bb_max, t.col(t.border), t.card_rounding, 0, t.border_size);
		// restrained accent edge, kept narrow so cards remain visually calm
		window->DrawList->AddRectFilled(cf.origin + ImVec2{18, 0}, cf.origin + ImVec2{72, 2}, t.col(t.accent), 1.f);
	}

	ImGui::PushID(id);
	window->ContentRegionRect.Max.x -= padding.x;
	window->WorkRect.Max.x -= padding.x;
	ImGui::Indent(padding.x);
	ImGui::SetCursorScreenPos({ImGui::GetCursorScreenPos().x, cf.origin.y + padding.y});
	ImGui::BeginGroup();
	return true;
}
} // namespace

bool begin_card(const char * id, const ImVec2 &)
{
	return begin_card_impl(id, metrics::card_padding);
}

bool begin_list_card(const char * id)
{
	return begin_card_impl(id, metrics::list_card_padding);
}

void end_card()
{
	ImGuiWindow * window = ImGui::GetCurrentWindow();
	const card_frame cf = card_stack.back();
	card_stack.pop_back();

	ImGui::EndGroup();
	ImGui::Unindent(cf.padding.x);
	window->ContentRegionRect.Max.x = cf.content_max_x_backup;
	window->WorkRect.Max.x = cf.work_max_x_backup;
	ImGui::PopID();

	const float height = ImGui::GetItemRectMax().y + cf.padding.y - cf.origin.y;
	card_heights[cf.id] = height;

	// reserve the full card extent so layout and scrolling account for it
	ImGui::SetCursorScreenPos(cf.origin);
	ImGui::Dummy({cf.width, height});
}

void row_separator()
{
	const theme & t = current();
	ImVec4 edge = t.border;
	edge.w *= background_alpha(); // match the panel/card opacity
	ImGui::PushStyleColor(ImGuiCol_Separator, edge);
	ImGui::Separator();
	ImGui::PopStyleColor();
}

float setting_label(const std::string & title, const std::string & description, float control_width)
{
	const theme & t = current();
	const ImGuiStyle & style = ImGui::GetStyle();

	const ImVec2 start = ImGui::GetCursorPos();
	const float avail = ImGui::GetContentRegionAvail().x;
	float text_w = avail - control_width - style.ItemSpacing.x;
	if (text_w < 1)
		text_w = avail;

	ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {style.ItemSpacing.x, metrics::label_line_gap});
	ImGui::PushTextWrapPos(start.x + text_w);
	ImGui::TextUnformatted(title.c_str());
	ImGui::PopTextWrapPos();
	if (not description.empty())
	{
		ImGui::PushFont(nullptr, style.FontSizeBase * metrics::font_description);
		ImGui::PushStyleColor(ImGuiCol_Text, t.text_muted);
		// never wider than half the row, wrap to as many lines as needed
		const float desc_w = ImMin(text_w, avail * 0.5f);
		ImGui::PushTextWrapPos(start.x + desc_w);
		ImGui::TextUnformatted(description.c_str());
		ImGui::PopTextWrapPos();
		ImGui::PopStyleColor();
		ImGui::PopFont();
	}
	ImGui::PopStyleVar();

	// right-aligned control slot, vertically centered against the label block
	const float label_h = ImGui::GetCursorPos().y - start.y;
	const float ctrl_h = ImGui::GetFrameHeight() * metrics::control_height;
	const float ctrl_y = start.y + ImMax(0.f, (label_h - ctrl_h) * 0.5f);
	ImGui::SetCursorPos({start.x + avail - control_width, ctrl_y});
	return start.y + label_h;
}

namespace
{
struct button_colors
{
	ImVec4 bg, bg_hovered, bg_active, text;
};

button_colors colors_for(button_style s, const theme & t)
{
	switch (s)
	{
		case button_style::primary:
			return {t.accent, t.accent_hovered, t.accent_active, t.on_accent};
		case button_style::secondary:
			return {t.control, t.control_hovered, t.control_active, t.text};
		case button_style::danger:
			return {t.danger, t.danger_hovered, t.danger, t.on_accent};
		case button_style::ghost:
			return {{0, 0, 0, 0}, t.control, t.control_active, t.text};
	}
	return {};
}
} // namespace

std::string icon_label(const char * icon, const std::string & label)
{
	return std::string(icon) + "  " + label;
}

float button_width(const std::string & label)
{
	const char * end = ImGui::FindRenderedTextEnd(label.c_str());
	return ImGui::CalcTextSize(label.c_str(), end).x + metrics::button_padding.x * 2;
}

float button_width(const char * icon, const std::string & label)
{
	ImGui::PushFont(nullptr, ImGui::GetFontSize() * metrics::button_label_glyph);
	const float icon_w = ImGui::CalcTextSize(icon).x;
	ImGui::PopFont();
	const float gap = ImGui::GetFontSize() * 0.4f;
	const char * end = ImGui::FindRenderedTextEnd(label.c_str());
	return icon_w + gap + ImGui::CalcTextSize(label.c_str(), end).x + metrics::button_padding.x * 2;
}

ImVec2 chip_size(const std::string & label)
{
	const ImVec2 ts = ImGui::CalcTextSize(label.c_str());
	return {ts.x + metrics::chip_padding.x * 2, ts.y + metrics::chip_padding.y * 2};
}

float chip_width(const std::string & label, bool dot, float height)
{
	// mirrors the footprint computed in chip()
	const ImVec2 ts = ImGui::CalcTextSize(label.c_str());
	const float pad_x = height > 0 ? metrics::chip_pill_padding_x : metrics::chip_padding.x;
	const float dot_r = ts.y * 0.26f;
	const float dot_gap = dot ? dot_r * 2 + pad_x * 0.55f : 0;
	return ts.x + pad_x * 2 + dot_gap;
}

bool button(const std::string & label, button_style s, const ImVec2 & size)
{
	ImGuiWindow * window = ImGui::GetCurrentWindow();
	if (window->SkipItems)
		return false;

	const theme & t = current();
	const ImGuiStyle & style = ImGui::GetStyle();

	// control height by default, width auto-fits the label
	const float h = size.y > 0 ? size.y : ImGui::GetFrameHeight() * metrics::control_height;
	const float w = size.x > 0 ? size.x : button_width(label);
	const ImVec2 pos = window->DC.CursorPos;
	const ImRect bb(pos, pos + ImVec2{w, h});
	ImGui::ItemSize(bb, style.FramePadding.y);
	const ImGuiID gid = window->GetID(label.c_str());
	if (not ImGui::ItemAdd(bb, gid))
		return false;

	const button_colors c = colors_for(s, t);
	bool hovered, held;
	const bool pressed = ImGui::ButtonBehavior(bb, gid, &hovered, &held);
	hover_haptic();
	const float hover_t = animate_scalar(gid, hovered ? 1.f : 0.f);
	const float held_t = held ? 1.f : 0.f;
	ImVec4 bg = mix(c.bg, c.bg_hovered, hover_t);
	bg = mix(bg, c.bg_active, held_t);
	ImDrawList * draw = window->DrawList;
	draw->AddRectFilled(bb.Min, bb.Max, t.col(bg), t.rounding);
	if (t.border_size > 0 and s != button_style::primary)
		draw->AddRect(bb.Min, bb.Max, t.col(t.border), t.rounding, 0, t.border_size);
	const char * label_end = ImGui::FindRenderedTextEnd(label.c_str());
	const ImVec2 ts = ImGui::CalcTextSize(label.c_str(), label_end);
	draw->AddText({bb.Min.x + (w - ts.x) * 0.5f, bb.Min.y + (h - ts.y) * 0.5f}, t.col(c.text), label.c_str(), label_end);
	return pressed;
}

bool button(const char * icon, const std::string & label, button_style s, const ImVec2 & size)
{
	ImGuiWindow * window = ImGui::GetCurrentWindow();
	if (window->SkipItems)
		return false;

	ImGuiContext & g = *GImGui;
	const ImGuiStyle & style = g.Style;
	const theme & t = current();

	const float h = size.y > 0 ? size.y : ImGui::GetFrameHeight() * metrics::control_height;
	const float glyph_size = ImGui::GetFontSize() * metrics::button_label_glyph;
	const float gap = ImGui::GetFontSize() * 0.4f;

	ImGui::PushFont(nullptr, glyph_size);
	const float icon_w = ImGui::CalcTextSize(icon).x;
	ImGui::PopFont();
	const char * label_end = ImGui::FindRenderedTextEnd(label.c_str());
	const ImVec2 label_size = ImGui::CalcTextSize(label.c_str(), label_end);
	const float content_w = icon_w + gap + label_size.x;
	const float w = size.x > 0 ? size.x : content_w + metrics::button_padding.x * 2;

	const ImVec2 pos = window->DC.CursorPos;
	const ImRect bb(pos, pos + ImVec2{w, h});
	ImGui::ItemSize(bb, style.FramePadding.y);
	const ImGuiID id = window->GetID(label.c_str());
	if (not ImGui::ItemAdd(bb, id))
		return false;

	bool hovered, held;
	const bool pressed = ImGui::ButtonBehavior(bb, id, &hovered, &held);
	hover_haptic();

	const button_colors c = colors_for(s, t);
	const float hover_t = animate_scalar(id, hovered ? 1.f : 0.f);
	const float held_t = held ? 1.f : 0.f;
	ImVec4 bg = mix(c.bg, c.bg_hovered, hover_t);
	bg = mix(bg, c.bg_active, held_t);

	ImDrawList * draw = window->DrawList;
	draw->AddRectFilled(bb.Min, bb.Max, t.col(bg), t.rounding);
	if (t.border_size > 0 and s != button_style::primary)
		draw->AddRect(bb.Min, bb.Max, t.col(t.border), t.rounding, 0, t.border_size);

	const ImU32 col = t.col(c.text);
	const ImVec2 center = bb.GetCenter();
	float x = center.x - content_w * 0.5f;

	// icon centred vertically on its ink box, like icon_button
	ImGui::PushFont(nullptr, glyph_size);
	unsigned int codepoint = 0;
	ImTextCharFromUtf8(&codepoint, icon, nullptr);
	const ImFontGlyph * glyph = ImGui::GetFontBaked()->FindGlyph(ImWchar(codepoint));
	if (glyph)
		draw->AddText({x, center.y - (glyph->Y0 + glyph->Y1) * 0.5f}, col, icon);
	else
		draw->AddText({x, center.y - glyph_size * 0.5f}, col, icon);
	ImGui::PopFont();

	x += icon_w + gap;
	draw->AddText({x, center.y - label_size.y * 0.5f}, col, label.c_str(), label_end);

	return pressed;
}

bool icon_button(const char * icon, const ImVec2 & size_arg, bool active, const std::string & tooltip)
{
	ImGuiWindow * window = ImGui::GetCurrentWindow();
	if (window->SkipItems)
		return false;

	ImGuiContext & g = *GImGui;
	const ImGuiStyle & style = g.Style;
	const theme & t = current();

	// square touch target, same height as the sliders and other controls
	const float side = ImGui::GetFrameHeight() * metrics::control_height;
	const ImVec2 size = size_arg.x > 0 ? size_arg : ImVec2{side, side};

	const ImVec2 pos = window->DC.CursorPos;
	const ImRect bb(pos, pos + size);
	ImGui::ItemSize(bb, style.FramePadding.y);
	const ImGuiID gid = window->GetID(icon);
	if (not ImGui::ItemAdd(bb, gid))
		return false;

	bool hovered, held;
	const bool pressed = ImGui::ButtonBehavior(bb, gid, &hovered, &held);
	hover_haptic();
	if (hovered)
		show_tooltip(tooltip);

	const float hover_t = animate_scalar(gid, hovered ? 1.f : 0.f);
	const float held_t = held ? 1.f : 0.f;
	ImVec4 bg = active ? t.accent : t.control;
	bg = mix(bg, active ? t.accent_hovered : t.control_hovered, hover_t);
	bg = mix(bg, active ? t.accent_active : t.control_active, held_t);

	ImDrawList * draw = window->DrawList;
	draw->AddRectFilled(bb.Min, bb.Max, t.col(bg), t.rounding);

	// centre the glyph on its ink box, not CalcTextSize
	const float glyph_size = size.y * metrics::icon_button_glyph;
	ImGui::PushFont(nullptr, glyph_size);
	const ImU32 col = t.col(active ? t.on_accent : t.text);
	const ImVec2 center = bb.GetCenter();

	unsigned int codepoint = 0;
	ImTextCharFromUtf8(&codepoint, icon, nullptr);
	const ImFontGlyph * glyph = ImGui::GetFontBaked()->FindGlyph(ImWchar(codepoint));
	if (glyph)
		draw->AddText({center.x - (glyph->X0 + glyph->X1) * 0.5f, center.y - (glyph->Y0 + glyph->Y1) * 0.5f}, col, icon);
	else
	{
		const ImVec2 ts = ImGui::CalcTextSize(icon);
		draw->AddText({center.x - ts.x * 0.5f, center.y - ts.y * 0.5f}, col, icon);
	}
	ImGui::PopFont();

	return pressed;
}

bool cancel_progress_button(const char * id, float fraction, const ImVec2 & size_arg, const std::string & tooltip)
{
	ImGuiWindow * window = ImGui::GetCurrentWindow();
	if (window->SkipItems)
		return false;

	ImGuiContext & g = *GImGui;
	const theme & t = current();

	const float side = ImGui::GetFrameHeight() * metrics::control_height;
	const ImVec2 size = size_arg.x > 0 ? size_arg : ImVec2{side, side};

	const ImVec2 pos = window->DC.CursorPos;
	const ImRect bb(pos, pos + size);
	ImGui::ItemSize(bb, g.Style.FramePadding.y);
	const ImGuiID gid = window->GetID(id);
	if (not ImGui::ItemAdd(bb, gid))
		return false;

	bool hovered, held;
	const bool pressed = ImGui::ButtonBehavior(bb, gid, &hovered, &held);
	hover_haptic();
	if (hovered)
		show_tooltip(tooltip);

	ImDrawList * draw = window->DrawList;
	const ImVec2 center = bb.GetCenter();
	const float thickness = ImMax(2.f, size.y * 0.09f);
	const float radius = ImMin(size.x, size.y) * 0.5f - thickness;

	// faint full ring as the track
	draw->PathArcTo(center, radius, 0, 2 * IM_PI, 48);
	draw->PathStroke(t.col(t.control), ImDrawFlags_None, thickness);

	const ImU32 accent = t.col(t.accent);
	if (fraction >= 0)
	{
		// determinate arc, clockwise from the top
		const float a0 = -IM_PI * 0.5f;
		draw->PathArcTo(center, radius, a0, a0 + ImClamp(fraction, 0.f, 1.f) * 2 * IM_PI, 48);
		draw->PathStroke(accent, ImDrawFlags_None, thickness);
	}
	else
	{
		// indeterminate: a rotating quarter arc
		const float a0 = float(g.Time) * 3.5f;
		draw->PathArcTo(center, radius, a0, a0 + IM_PI * 0.5f, 24);
		draw->PathStroke(accent, ImDrawFlags_None, thickness);
	}

	// stop glyph in the centre, brightening on hover to read as a cancel target
	const float glyph_size = size.y * metrics::icon_button_glyph * 0.7f;
	ImGui::PushFont(nullptr, glyph_size);
	const ImU32 col = t.col(hovered or held ? t.text : t.text_muted);
	unsigned int codepoint = 0;
	ImTextCharFromUtf8(&codepoint, ICON_FA_STOP, nullptr);
	const ImFontGlyph * glyph = ImGui::GetFontBaked()->FindGlyph(ImWchar(codepoint));
	if (glyph)
		draw->AddText({center.x - (glyph->X0 + glyph->X1) * 0.5f, center.y - (glyph->Y0 + glyph->Y1) * 0.5f}, col, ICON_FA_STOP);
	ImGui::PopFont();

	return pressed;
}

// Horizontal space a trailing reset slot consumes, gap included
float reset_slot_width()
{
	return ImGui::GetFrameHeight() * metrics::control_height + ImGui::GetStyle().ItemSpacing.x;
}

// trailing reset button on a value control's row, empty slot when not shown so the right edge stays put
static bool reset_slot(const char * id, bool show)
{
	const float side = ImGui::GetFrameHeight() * metrics::control_height;
	ImGui::SameLine(0, ImGui::GetStyle().ItemSpacing.x);
	if (not show)
	{
		ImGui::Dummy({side, side});
		return false;
	}
	ImGui::PushID(id);
	const bool clicked = icon_button(ICON_FA_ARROW_ROTATE_LEFT, {side, side}, false, "Reset to default");
	ImGui::PopID();
	return clicked;
}

bool toggle(const char * id, bool * v, const bool * default_value)
{
	ImGuiWindow * window = ImGui::GetCurrentWindow();
	if (window->SkipItems)
		return false;

	const theme & t = current();
	const ImGuiStyle & style = ImGui::GetStyle();

	// match the height of the other controls
	const float height = ImGui::GetFrameHeight() * metrics::control_height;
	const float width = height * metrics::toggle_aspect;
	const float radius = height * 0.5f;

	const ImVec2 pos = window->DC.CursorPos;
	const ImRect bb(pos, pos + ImVec2(width, height));
	ImGui::ItemSize(bb, style.FramePadding.y);
	const ImGuiID gid = window->GetID(id);
	if (not ImGui::ItemAdd(bb, gid))
		return false;

	bool hovered, held;
	bool changed = ImGui::ButtonBehavior(bb, gid, &hovered, &held);
	hover_haptic();
	if (changed)
		*v = not *v;

	const float hover_t = animate_scalar(gid, hovered ? 1.f : 0.f);
	const float state_t = animate_scalar(gid ^ 0x6f3a2b1u, *v ? 1.f : 0.f, 18.f);
	ImVec4 track = mix(t.control, t.accent, state_t);
	track = mix(track, *v ? t.accent_hovered : t.control_hovered, hover_t * 0.45f);

	ImDrawList * draw = window->DrawList;
	draw->AddRectFilled(bb.Min, bb.Max, t.col(track), radius);
	const float cx = bb.Min.x + radius + (bb.Max.x - bb.Min.x - 2 * radius) * state_t;
	draw->AddCircleFilled({cx, bb.Min.y + radius}, radius - metrics::toggle_knob_inset, t.col(t.on_accent));

	if (default_value and reset_slot(id, *v != *default_value))
	{
		*v = *default_value;
		changed = true;
	}

	return changed;
}

bool slider_int(const char * id, int * v, int v_min, int v_max, const char * format, const ImVec2 & size_arg, const int * default_value)
{
	ImGuiWindow * window = ImGui::GetCurrentWindow();
	if (window->SkipItems)
		return false;

	ImGuiContext & g = *GImGui;
	const ImGuiStyle & style = g.Style;
	const theme & t = current();
	const ImGuiID gid = window->GetID(id);

	ImVec2 size = ImGui::CalcItemSize(size_arg, ImGui::CalcItemWidth(), ImGui::GetFrameHeight() * metrics::control_height);
	if (default_value)
		size.x -= reset_slot_width();
	const ImVec2 pos = window->DC.CursorPos;
	const ImRect bb(pos, pos + size);
	ImGui::ItemSize(bb, style.FramePadding.y);
	if (not ImGui::ItemAdd(bb, gid, &bb))
		return false;

	// activation, mirrors ImGui::SliderScalar
	const bool hovered = ImGui::ItemHoverable(bb, gid, g.LastItemData.ItemFlags);
	hover_haptic();
	const bool clicked = hovered and ImGui::IsMouseClicked(ImGuiMouseButton_Left, ImGuiInputFlags_None, gid);
	if (clicked or g.NavActivateId == gid)
	{
		if (clicked)
			ImGui::SetKeyOwner(ImGuiKey_MouseLeft, gid);
		ImGui::SetActiveID(gid, window);
		ImGui::SetFocusID(gid, window);
		ImGui::FocusWindow(window);
		g.ActiveIdUsingNavDirMask |= (1 << ImGuiDir_Left) | (1 << ImGuiDir_Right);
	}

	ImRect grab;
	const bool changed = ImGui::SliderBehavior(bb, gid, ImGuiDataType_S32, v, &v_min, &v_max, format, ImGuiSliderFlags_None, &grab);
	if (changed)
		ImGui::MarkItemEdited(gid);

	const float fraction = v_max > v_min ? float(*v - v_min) / float(v_max - v_min) : 0;
	const float fill_x = bb.Min.x + (bb.Max.x - bb.Min.x) * fraction;

	ImDrawList * draw = window->DrawList;
	draw->AddRectFilled(bb.Min, bb.Max, t.col(t.control), t.rounding);
	if (fraction > 0)
	{
		// round the right corners too once the fill reaches the box edge
		const ImDrawFlags fill_flags = fill_x >= bb.Max.x - t.rounding ? ImDrawFlags_RoundCornersAll : ImDrawFlags_RoundCornersLeft;
		draw->AddRectFilled(bb.Min, {fill_x, bb.Max.y}, t.col(t.accent_active), t.rounding, fill_flags);
	}
	// bright grab at the fill edge
	const float grab_w = metrics::slider_grab_width;
	const float gx = ImClamp(fill_x, bb.Min.x + grab_w * 0.5f, bb.Max.x - grab_w * 0.5f);
	draw->AddRectFilled({gx - grab_w * 0.5f, bb.Min.y}, {gx + grab_w * 0.5f, bb.Max.y}, t.col(t.accent), t.rounding);

	char buf[64];
	std::snprintf(buf, sizeof(buf), format, *v);
	const ImVec2 ts = ImGui::CalcTextSize(buf);
	draw->AddText({bb.Min.x + (size.x - ts.x) * 0.5f, bb.Min.y + (size.y - ts.y) * 0.5f}, t.col(t.text), buf);

	bool reset = false;
	if (default_value and reset_slot(id, *v != *default_value))
	{
		*v = *default_value;
		reset = true;
	}

	return changed or reset;
}

bool segmented(const char * id, const std::vector<std::string> & options, int * selected, const ImVec2 & size_arg, const int * default_value)
{
	ImGuiWindow * window = ImGui::GetCurrentWindow();
	if (window->SkipItems or options.empty())
		return false;

	ImGuiContext & g = *GImGui;
	const ImGuiStyle & style = g.Style;
	const theme & t = current();

	ImVec2 size = ImGui::CalcItemSize(size_arg, ImGui::CalcItemWidth(), ImGui::GetFrameHeight() * metrics::control_height);
	if (default_value)
		size.x -= reset_slot_width();
	const ImVec2 pos = window->DC.CursorPos;
	const ImRect bb(pos, pos + size);
	ImGui::ItemSize(bb, style.FramePadding.y);
	const ImGuiID gid = window->GetID(id);
	if (not ImGui::ItemAdd(bb, gid))
		return false;

	const bool hovered = ImGui::ItemHoverable(bb, gid, g.LastItemData.ItemFlags);
	hover_haptic();
	// claim the active id on press so a drag here scrolls nothing (see ScrollWhenDragging)
	if (hovered and ImGui::IsMouseClicked(ImGuiMouseButton_Left))
	{
		ImGui::SetActiveID(gid, window);
		ImGui::FocusWindow(window);
	}
	if (g.ActiveId == gid and not g.IO.MouseDown[ImGuiMouseButton_Left])
		ImGui::ClearActiveID();

	ImDrawList * draw = window->DrawList;
	draw->AddRectFilled(bb.Min, bb.Max, t.col(t.control), t.rounding);

	const int n = int(options.size());
	const float seg_w = size.x / n;
	const float pad = metrics::segmented_inset;
	bool changed = false;
	const float active_pos = animate_scalar(gid, float(*selected), 18.f);
	const ImRect active_seg({bb.Min.x + seg_w * active_pos, bb.Min.y}, {bb.Min.x + seg_w * (active_pos + 1.f), bb.Max.y});
	draw->AddRectFilled(active_seg.Min + ImVec2(pad, pad), active_seg.Max - ImVec2(pad, pad), t.col(t.accent), t.rounding);

	for (int i = 0; i < n; ++i)
	{
		const ImRect seg({bb.Min.x + seg_w * i, bb.Min.y}, {bb.Min.x + seg_w * (i + 1), bb.Max.y});
		const bool seg_hovered = hovered and seg.Contains(g.IO.MousePos);
		if (seg_hovered and ImGui::IsMouseClicked(ImGuiMouseButton_Left))
		{
			*selected = i;
			changed = true;
		}

		const bool active = *selected == i;
		if (not active and seg_hovered)
			draw->AddRectFilled(seg.Min + ImVec2(pad, pad), seg.Max - ImVec2(pad, pad), t.col(t.control_hovered), t.rounding);

		const char * label = options[i].c_str();
		const ImVec2 ts = ImGui::CalcTextSize(label);
		draw->AddText({seg.Min.x + (seg_w - ts.x) * 0.5f, seg.Min.y + (size.y - ts.y) * 0.5f}, t.col(active ? t.on_accent : t.text), label);
	}

	if (default_value and reset_slot(id, *selected != *default_value))
	{
		*selected = *default_value;
		changed = true;
	}

	return changed;
}

namespace
{
// where combo modals open, in imgui display coords, set each frame by the lobby
ImVec2 popup_center = {0, 0};
// height of the popup layer, caps tall combo lists so they scroll, 0 = unbounded
float popup_available_height = 0;

// one selectable option inside the combo modal, tall full-width touch target with an optional description
bool combo_row(const combo_item & item, bool selected, bool checked)
{
	ImGuiWindow * window = ImGui::GetCurrentWindow();
	const theme & t = current();

	const float row_h = ImGui::GetFrameHeight() * metrics::combo_row_height;
	const float row_w = ImGui::GetContentRegionAvail().x;
	const ImVec2 pos = window->DC.CursorPos;
	const ImRect bb(pos, pos + ImVec2(row_w, row_h));
	ImGui::ItemSize(bb, 0);
	const ImGuiID gid = window->GetID(item.name);
	if (not ImGui::ItemAdd(bb, gid))
		return false;

	bool hovered, held;
	const bool pressed = ImGui::ButtonBehavior(bb, gid, &hovered, &held);
	hover_haptic();

	ImVec4 bg = selected ? t.control_active : t.card;
	if (held)
		bg = t.control_active;
	else if (hovered)
		bg = t.control_hovered;

	ImDrawList * draw = window->DrawList;
	draw->AddRectFilled(bb.Min, bb.Max, t.col(bg), t.rounding);
	if (selected)
		draw->AddRectFilled(bb.Min, {bb.Min.x + metrics::combo_padding.x * 0.25f, bb.Max.y}, t.col(t.accent), t.rounding, ImDrawFlags_RoundCornersLeft);

	if (checked)
	{
		const ImVec2 cs = ImGui::CalcTextSize(ICON_FA_CHECK);
		draw->AddText({bb.Max.x - metrics::combo_padding.x - cs.x, bb.Min.y + (row_h - cs.y) * 0.5f}, t.col(t.accent), ICON_FA_CHECK);
	}

	const float tx = bb.Min.x + metrics::combo_padding.x;
	const ImU32 name_col = t.col(t.text);
	const ImVec2 nts = ImGui::CalcTextSize(item.name);

	if (item.description and item.description[0])
	{
		const float desc_size = ImGui::GetStyle().FontSizeBase * metrics::font_description;
		ImGui::PushFont(nullptr, desc_size);
		const ImVec2 dts = ImGui::CalcTextSize(item.description);
		const float total = nts.y + metrics::label_line_gap + dts.y;
		const float y = bb.Min.y + (row_h - total) * 0.5f;
		draw->AddText({tx, y + nts.y + metrics::label_line_gap}, t.col(t.text_muted), item.description);
		ImGui::PopFont();
		draw->AddText({tx, y}, name_col, item.name);
	}
	else
	{
		draw->AddText({tx, bb.Min.y + (row_h - nts.y) * 0.5f}, name_col, item.name);
	}

	return pressed;
}

bool combo_impl(const char * id, const std::string & title, const char * preview, const std::vector<combo_item> & items, int * selected, std::vector<char> * selected_multi, float width, const int * default_value, const std::vector<char> * default_value_multi, bool close_on_click)
{
	ImGuiWindow * window = ImGui::GetCurrentWindow();
	if (window->SkipItems or items.empty())
		return false;

	const ImGuiStyle & style = ImGui::GetStyle();
	const theme & t = current();
	const ImGuiID gid = window->GetID(id);

	// closed box, same height as the slider so setting rows line up
	ImVec2 size = ImGui::CalcItemSize({width, 0}, ImGui::CalcItemWidth(), ImGui::GetFrameHeight() * metrics::control_height);
	if (default_value or default_value_multi)
		size.x -= reset_slot_width();
	const ImVec2 pos = window->DC.CursorPos;
	const ImRect bb(pos, pos + size);
	ImGui::ItemSize(bb, style.FramePadding.y);
	if (not ImGui::ItemAdd(bb, gid))
		return false;

	bool hovered, held;
	const bool clicked = ImGui::ButtonBehavior(bb, gid, &hovered, &held);
	hover_haptic();

	ImVec4 box = t.control;
	if (held)
		box = t.control_active;
	else if (hovered)
		box = t.control_hovered;

	ImDrawList * draw = window->DrawList;
	draw->AddRectFilled(bb.Min, bb.Max, t.col(box), t.rounding);
	if (t.border_size > 0)
		draw->AddRect(bb.Min, bb.Max, t.col(t.border), t.rounding, 0, t.border_size);

	const int sel = selected ? ImClamp(*selected, 0, int(items.size()) - 1) : -1;

	// preview, left aligned and clipped to the box
	const ImVec2 ts = ImGui::CalcTextSize(preview);
	const float pad_x = metrics::combo_padding.x;
	draw->PushClipRect(bb.Min, bb.Max, true);
	draw->AddText({bb.Min.x + pad_x, bb.Min.y + (size.y - ts.y) * 0.5f}, t.col(t.text), preview);
	draw->PopClipRect();

	// chevron on the right
	const float cx = bb.Max.x - pad_x - metrics::combo_chevron * 0.5f;
	const float cy = bb.Min.y + size.y * 0.5f;
	const float r = metrics::combo_chevron * 0.5f;
	draw->AddTriangleFilled({cx - r, cy - r * 0.4f}, {cx + r, cy - r * 0.4f}, {cx, cy + r * 0.6f}, t.col(t.text_muted));

	char popup_id[64];
	std::snprintf(popup_id, sizeof(popup_id), "##combo_%08X", gid);
	if (clicked)
		ImGui::OpenPopup(popup_id);

	ImGui::SetNextWindowPos(popup_center, ImGuiCond_Always, {0.5f, 0.5f});
	ImGui::SetNextWindowSize({ImMax(size.x, metrics::combo_modal_min_width), 0});

	ImGui::PushStyleColor(ImGuiCol_PopupBg, t.card);
	ImGui::PushStyleColor(ImGuiCol_Border, t.border);
	ImGui::PushStyleColor(ImGuiCol_Text, t.text);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, t.card_rounding);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, metrics::card_padding);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, t.border_size > 0 ? t.border_size : 1);

	bool changed = false;
	if (ImGui::BeginPopupModal(popup_id, nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_AlwaysAutoResize))
	{
		if (not title.empty())
		{
			ImGui::PushStyleColor(ImGuiCol_Text, t.text_muted);
			ImGui::TextUnformatted(title.c_str());
			ImGui::PopStyleColor();
			ImGui::Dummy({0, style.ItemSpacing.y});
		}

		// cap the list height so a long list scrolls inside the modal instead of clipping
		const float row_h = ImGui::GetFrameHeight() * metrics::combo_row_height;
		// include the inter-row gaps or a scrollbar shows even when it all fits
		float list_h = int(items.size()) * row_h + ImMax(0, int(items.size()) - 1) * style.ItemSpacing.y;
		if (popup_available_height > 0)
		{
			const float title_h = title.empty() ? 0 : ImGui::GetTextLineHeight() + style.ItemSpacing.y;
			const float max_list = popup_available_height * 0.9f - 2 * metrics::card_padding.y - title_h;
			list_h = ImMin(list_h, ImMax(max_list, row_h));
		}

		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0, 0});
		ImGui::BeginChild("##list", {0, list_h});
		for (int i = 0; i < int(items.size()); ++i)
		{
			const bool is_selected = i == sel;
			const bool is_selected_multi = selected_multi and i < selected_multi->size() and (*selected_multi)[i];
			if (combo_row(items[i], is_selected, is_selected_multi))
			{
				if (selected)
					*selected = i;

				if (selected_multi)
				{
					if (selected_multi->size() <= i)
						selected_multi->resize(i + 1);
					(*selected_multi)[i] = not(*selected_multi)[i];
				}

				changed = true;
				if (close_on_click)
					ImGui::CloseCurrentPopup();
			}
			if (is_selected and ImGui::IsWindowAppearing())
				ImGui::SetScrollHereY(0.5f); // reveal the current value when opening
		}
		ImGui::EndChild();
		ImGui::PopStyleVar();

		// click outside dismisses, AllowWhenBlockedByActiveItem so pressing a row still counts as inside
		if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) and not ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows | ImGuiHoveredFlags_AllowWhenBlockedByActiveItem))
			ImGui::CloseCurrentPopup();

		ImGui::EndPopup();
	}

	ImGui::PopStyleVar(3);
	ImGui::PopStyleColor(3);

	bool show_reset_slot = (selected and default_value and *selected != *default_value) or
	                       (selected_multi and default_value_multi and *selected_multi != *default_value_multi);

	if (reset_slot(id, show_reset_slot))
	{
		if (selected and default_value)
			*selected = *default_value;

		if (selected_multi and default_value_multi)
			*selected_multi = *default_value_multi;

		changed = true;
	}

	return changed;
}
} // namespace

void set_popup_center(const ImVec2 & center, float available_height)
{
	popup_center = center;
	popup_available_height = available_height;
}

bool combo(const char * id, const std::string & title, const std::vector<combo_item> & items, int * selected, float width, const int * default_value)
{
	const int sel = ImClamp(*selected, 0, int(items.size()) - 1);
	const char * preview = items[sel].name;

	return combo_impl(id, title, preview, items, selected, nullptr, width, default_value, nullptr, true);
}

bool combo_multi(const char * id, const std::string & title, const std::string & preview, const std::vector<combo_item> & items, std::vector<char> * selected, float width, const std::vector<char> * default_value)
{
	return combo_impl(id, title, preview.c_str(), items, nullptr, selected, width, nullptr, default_value, false);
}

void chip(const std::string & label, chip_style style, bool dot, float height)
{
	ImGuiWindow * window = ImGui::GetCurrentWindow();
	if (window->SkipItems)
		return;

	const theme & t = current();

	// foreground (text/dot/icon) and the pill fill
	ImVec4 fg = t.text;
	ImVec4 bg = t.control;
	switch (style)
	{
		case chip_style::neutral:
			break;
		case chip_style::muted:
			fg = t.text_muted;
			break;
		case chip_style::accent:
			fg = t.accent;
			bg = {t.accent.x, t.accent.y, t.accent.z, 0.16f};
			break;
		case chip_style::success:
			fg = t.success;
			bg = {t.success.x, t.success.y, t.success.z, 0.16f};
			break;
		case chip_style::warning:
			fg = t.warning;
			bg = {t.warning.x, t.warning.y, t.warning.z, 0.16f};
			break;
		case chip_style::danger:
			fg = t.danger;
			bg = {t.danger.x, t.danger.y, t.danger.z, 0.16f};
			break;
	}

	const ImVec2 ts = ImGui::CalcTextSize(label.c_str());
	const ImVec2 pad = metrics::chip_padding;
	// tall pill chips (height override) get roomier sides
	const float pad_x = height > 0 ? metrics::chip_pill_padding_x : pad.x;
	const float dot_r = ts.y * 0.26f;
	const float dot_gap = dot ? dot_r * 2 + pad_x * 0.55f : 0;
	const ImVec2 size = {ts.x + pad_x * 2 + dot_gap, height > 0 ? height : ts.y + pad.y * 2};

	const ImVec2 pos = window->DC.CursorPos;
	const ImRect bb(pos, pos + size);
	ImGui::ItemSize(bb);
	if (not ImGui::ItemAdd(bb, 0))
		return;

	ImDrawList * draw = window->DrawList;
	draw->AddRectFilled(bb.Min, bb.Max, t.col(bg), size.y * 0.5f);

	float tx = bb.Min.x + pad_x;
	if (dot)
	{
		draw->AddCircleFilled({tx + dot_r, bb.Min.y + size.y * 0.5f}, dot_r, t.col(fg));
		tx += dot_r * 2 + pad_x * 0.55f;
	}
	draw->AddText({tx, bb.Min.y + (size.y - ts.y) * 0.5f}, t.col(fg), label.c_str());
}

void section_header(const std::string & title, const std::string & subtitle)
{
	const theme & t = current();
	const ImGuiStyle & style = ImGui::GetStyle();
	ImGui::PushFont(nullptr, style.FontSizeBase * metrics::font_modal_title);
	ImGui::TextUnformatted(title.c_str());
	ImGui::PopFont();
	if (not subtitle.empty())
	{
		ImGui::PushFont(nullptr, style.FontSizeBase * metrics::font_description);
		ImGui::PushStyleColor(ImGuiCol_Text, t.text_muted);
		ImGui::TextUnformatted(subtitle.c_str());
		ImGui::PopStyleColor();
		ImGui::PopFont();
	}
	ImGui::Dummy({0, metrics::section_gap * 0.25f});
}

bool disclosure(const char * id,
                const char * icon,
                const std::string & title,
                const std::string & subtitle,
                bool * open,
                const std::string & status,
                chip_style status_style)
{
	ImGuiWindow * window = ImGui::GetCurrentWindow();
	if (window->SkipItems)
		return false;

	const theme & t = current();
	const ImGuiStyle & style = ImGui::GetStyle();
	const float h = ImGui::GetFrameHeight() * metrics::disclosure_height;
	const ImVec2 pos = window->DC.CursorPos;
	const ImRect bb(pos, pos + ImVec2{ImGui::GetContentRegionAvail().x, h});
	ImGui::ItemSize(bb, style.FramePadding.y);
	const ImGuiID gid = window->GetID(id);
	if (not ImGui::ItemAdd(bb, gid))
		return false;

	bool hovered, held;
	const bool pressed = ImGui::ButtonBehavior(bb, gid, &hovered, &held);
	hover_haptic();
	if (pressed)
		*open = not *open;

	const float hover_t = animate_scalar(gid, hovered ? 1.f : 0.f);
	const float open_t = animate_scalar(gid ^ 0x18d932u, *open ? 1.f : 0.f, 16.f);
	ImVec4 bg = mix(t.card, t.card_hovered, hover_t);
	bg = mix(bg, t.control, 0.30f * open_t);
	ImDrawList * draw = window->DrawList;
	draw->AddRectFilled(bb.Min, bb.Max, t.col(bg), t.card_rounding);
	draw->AddRect(bb.Min, bb.Max, t.col(*open ? t.accent : t.border), t.card_rounding, 0, t.border_size);
	if (open_t > 0.001f)
		draw->AddRectFilled(bb.Min,
		                    {bb.Min.x + 4.f, bb.Max.y},
		                    t.col(t.accent),
		                    t.card_rounding,
		                    ImDrawFlags_RoundCornersLeft);

	const float pad = metrics::card_padding.x;
	const float icon_box = h - 2.f * metrics::list_row_pad;
	const ImVec2 icon_min{bb.Min.x + pad, bb.Min.y + metrics::list_row_pad};
	const ImVec2 icon_max = icon_min + ImVec2{icon_box, icon_box};
	draw->AddRectFilled(icon_min, icon_max, t.col(*open ? t.accent : t.control), t.rounding);
	if (icon and icon[0])
	{
		const ImVec2 icon_size = ImGui::CalcTextSize(icon);
		draw->AddText(icon_min + (ImVec2{icon_box, icon_box} - icon_size) * 0.5f,
		              t.col(*open ? t.on_accent : t.text_muted),
		              icon);
	}

	const float tx = icon_max.x + metrics::list_row_pad;
	const ImVec2 title_size = ImGui::CalcTextSize(title.c_str());
	const float desc_font_size = style.FontSizeBase * metrics::font_description;
	ImGui::PushFont(nullptr, desc_font_size);
	const ImVec2 subtitle_size = ImGui::CalcTextSize(subtitle.c_str());
	const float text_height = title_size.y + (subtitle.empty() ? 0.f : metrics::label_line_gap + subtitle_size.y);
	const float text_y = bb.Min.y + (h - text_height) * 0.5f;
	if (not subtitle.empty())
		draw->AddText({tx, text_y + title_size.y + metrics::label_line_gap},
		              t.col(t.text_muted),
		              subtitle.c_str());
	ImGui::PopFont();
	draw->AddText({tx, text_y}, t.col(t.text), title.c_str());

	const float arrow_space = 42.f;
	if (not status.empty())
	{
		ImVec4 status_fg = t.text;
		ImVec4 status_bg = t.control;
		switch (status_style)
		{
			case chip_style::accent:
				status_fg = t.accent;
				status_bg = {t.accent.x, t.accent.y, t.accent.z, 0.16f};
				break;
			case chip_style::success:
				status_fg = t.success;
				status_bg = {t.success.x, t.success.y, t.success.z, 0.16f};
				break;
			case chip_style::warning:
				status_fg = t.warning;
				status_bg = {t.warning.x, t.warning.y, t.warning.z, 0.16f};
				break;
			case chip_style::danger:
				status_fg = t.danger;
				status_bg = {t.danger.x, t.danger.y, t.danger.z, 0.16f};
				break;
			case chip_style::muted:
				status_fg = t.text_muted;
				break;
			case chip_style::neutral:
				break;
		}
		const ImVec2 status_text = ImGui::CalcTextSize(status.c_str());
		const ImVec2 status_size{status_text.x + metrics::chip_padding.x * 2.f,
		                         status_text.y + metrics::chip_padding.y * 2.f};
		const ImVec2 status_min{bb.Max.x - pad - arrow_space - status_size.x,
		                        bb.Min.y + (h - status_size.y) * 0.5f};
		draw->AddRectFilled(status_min, status_min + status_size, t.col(status_bg), status_size.y * 0.5f);
		draw->AddText(status_min + (status_size - status_text) * 0.5f,
		              t.col(status_fg),
		              status.c_str());
	}

	const ImVec2 center{bb.Max.x - pad - arrow_space * 0.5f, bb.GetCenter().y};
	const float r = 7.f;
	const ImVec2 closed_a{center.x - r * 0.45f, center.y - r};
	const ImVec2 closed_b{center.x - r * 0.45f, center.y + r};
	const ImVec2 closed_c{center.x + r * 0.65f, center.y};
	const ImVec2 open_a{center.x - r, center.y - r * 0.45f};
	const ImVec2 open_b{center.x + r, center.y - r * 0.45f};
	const ImVec2 open_c{center.x, center.y + r * 0.65f};
	auto tween = [open_t](const ImVec2 & a, const ImVec2 & b) { return a + (b - a) * open_t; };
	draw->AddTriangleFilled(tween(closed_a, open_a),
	                        tween(closed_b, open_b),
	                        tween(closed_c, open_c),
	                        t.col(t.text_muted));

	return pressed;
}

void nav_section(const std::string & label)
{
	const theme & t = current();
	const ImGuiStyle & style = ImGui::GetStyle();

	ImGui::Dummy({0, metrics::nav_section_gap}); // gap above the section
	ImGui::Indent(metrics::combo_padding.x);     // align with the nav_item icons
	ImGui::PushFont(nullptr, style.FontSizeBase * metrics::font_description);
	ImGui::PushStyleColor(ImGuiCol_Text, t.text_muted);
	ImGui::TextUnformatted(label.c_str());
	ImGui::PopStyleColor();
	ImGui::PopFont();
	const ImVec2 marker = ImGui::GetItemRectMin();
	ImGui::GetWindowDrawList()->AddRectFilled(marker - ImVec2{metrics::combo_padding.x, -2}, marker - ImVec2{metrics::combo_padding.x - 3, -style.FontSizeBase * metrics::font_description}, t.col(t.accent), 1.5f);
	ImGui::Unindent(metrics::combo_padding.x);
	ImGui::Dummy({0, 4});
}

bool nav_item(const char * icon, const std::string & label, bool selected)
{
	ImGuiWindow * window = ImGui::GetCurrentWindow();
	if (window->SkipItems)
		return false;

	const theme & t = current();
	const ImGuiStyle & style = ImGui::GetStyle();

	const float h = ImGui::GetFrameHeight() * metrics::control_height;
	const float w = ImGui::GetContentRegionAvail().x;
	const ImVec2 pos = window->DC.CursorPos;
	const ImRect bb(pos, pos + ImVec2(w, h));
	ImGui::ItemSize(bb, style.FramePadding.y);
	const ImGuiID gid = window->GetID(label.c_str());
	if (not ImGui::ItemAdd(bb, gid))
		return false;

	bool hovered, held;
	const bool pressed = ImGui::ButtonBehavior(bb, gid, &hovered, &held);
	hover_haptic();

	ImDrawList * draw = window->DrawList;
	const float lum = t.background.x * 0.299f + t.background.y * 0.587f + t.background.z * 0.114f;
	const float hover_t = animate_scalar(gid, hovered ? 1.f : 0.f);
	const float selected_t = animate_scalar(gid ^ 0x42a7d1u, selected ? 1.f : 0.f, 12.f);
	if (selected_t > 0.001f)
	{
		// Active rows use a translucent surface and a bright edge, avoiding a
		// wall of saturated colour while keeping focus obvious.
		ImVec4 active_bg = mix(t.control, t.accent, 0.16f * selected_t);
		draw->AddRectFilled(bb.Min, bb.Max, t.col(active_bg), t.rounding);
		const float edge_w = 3.f + 3.f * selected_t;
		draw->AddRectFilled(bb.Min, {bb.Min.x + edge_w, bb.Max.y}, t.col(t.accent), t.rounding, ImDrawFlags_RoundCornersLeft);
	}
	if (hover_t > 0.001f and not selected)
	{
		// lighten on dark themes, darken on light ones, the framebuffer blends in linear
		// light, so solve for the alpha that yields a constant perceived (gamma-space) step
		const float step = lum < 0.5f ? 0.10f : 0.06f; // perceptual lightness delta, stronger on dark
		auto to_linear = [](float c) { return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f); };
		const float overlay = lum < 0.5f ? 1.f : 0.f; // lighten vs darken
		const float ys = to_linear(lum);
		const float yt = to_linear(std::clamp(lum + (overlay - lum) / std::abs(overlay - lum) * step, 0.f, 1.f));
		const float a = (yt - ys) / (overlay - ys);
		draw->AddRectFilled(bb.Min, bb.Max, t.col({overlay, overlay, overlay, a * hover_t}), t.rounding);
	}

	const float pad = metrics::combo_padding.x;
	const float icon_w = ImGui::GetFontSize() * 1.5f; // icon column

	const ImVec2 its = ImGui::CalcTextSize(icon);
	draw->AddText({bb.Min.x + pad + (icon_w - its.x) * 0.5f, bb.Min.y + (h - its.y) * 0.5f}, t.col(selected ? t.accent : t.text_muted), icon);

	const ImVec2 ls = ImGui::CalcTextSize(label.c_str());
	draw->AddText({bb.Min.x + pad + icon_w + pad * 0.5f, bb.Min.y + (h - ls.y) * 0.5f}, t.col(selected ? t.text : t.text), label.c_str());

	return pressed;
}

static int input_text_resize(ImGuiInputTextCallbackData * data)
{
	if (data->EventFlag == ImGuiInputTextFlags_CallbackResize)
	{
		std::string & s = *static_cast<std::string *>(data->UserData);
		s.resize(data->BufTextLen);
		data->Buf = s.data();
	}
	return 0;
}

// common frame styling for the input widgets, paired with pop_input_style
static void push_input_style(float height)
{
	const theme & t = current();
	ImGui::PushStyleColor(ImGuiCol_FrameBg, t.control);
	ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, t.control_hovered);
	ImGui::PushStyleColor(ImGuiCol_FrameBgActive, t.control_active);
	ImGui::PushStyleColor(ImGuiCol_Text, t.text);
	ImGui::PushStyleColor(ImGuiCol_TextDisabled, t.text_muted);
	ImGui::PushStyleColor(ImGuiCol_Border, t.border);
	ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, t.rounding);
	ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, t.border_size);
	ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {metrics::input_padding_x, (height - ImGui::GetFontSize()) * 0.5f});
}

static void pop_input_style()
{
	ImGui::PopStyleVar(3);
	ImGui::PopStyleColor(6);
}

bool input_text(const char * id, std::string & text, const char * hint, float width)
{
	push_input_style(ImGui::GetFrameHeight() * metrics::control_height);
	const bool changed = ImGui::InputTextEx(id, hint, text.data(), int(text.size()) + 1, {width, 0}, ImGuiInputTextFlags_CallbackResize, input_text_resize, &text);
	pop_input_style();
	hover_haptic();
	return changed;
}

bool input_int(const char * id, int * v, int step, int v_min, int v_max, float width)
{
	const ImGuiStyle & style = ImGui::GetStyle();
	const float side = ImGui::GetFrameHeight() * metrics::control_height;
	const float gap = style.ItemSpacing.x;
	const float total = width > 0 ? width : ImGui::CalcItemWidth();
	const float field_w = total - 2 * (side + gap);

	ImGui::PushID(id);

	push_input_style(side);
	ImGui::SetNextItemWidth(field_w);
	bool changed = ImGui::InputInt("##field", v, 0, 0, ImGuiInputTextFlags_CharsDecimal); // step 0 hides native buttons
	pop_input_style();
	hover_haptic();

	ImGui::SameLine(0, gap);
	if (icon_button(ICON_FA_MINUS, {side, side}))
	{
		*v -= step;
		changed = true;
	}
	ImGui::SameLine(0, gap);
	if (icon_button(ICON_FA_PLUS, {side, side}))
	{
		*v += step;
		changed = true;
	}

	if (v_min != v_max)
		*v = ImClamp(*v, v_min, v_max);

	ImGui::PopID();
	return changed;
}

bool begin_modal(const char * id, const std::string & title, float width)
{
	const theme & t = current();
	const ImGuiStyle & style = ImGui::GetStyle();

	ImGui::SetNextWindowPos(popup_center, ImGuiCond_Always, {0.5f, 0.5f});
	if (width > 0)
		ImGui::SetNextWindowSize({width, 0});

	ImGui::PushStyleColor(ImGuiCol_PopupBg, t.card);
	ImGui::PushStyleColor(ImGuiCol_Border, t.border);
	ImGui::PushStyleColor(ImGuiCol_Text, t.text);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, t.card_rounding);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, metrics::card_padding);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, t.border_size > 0 ? t.border_size : 1);

	ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove;
	if (width <= 0)
		flags |= ImGuiWindowFlags_AlwaysAutoResize;

	const bool open = ImGui::BeginPopupModal(id, nullptr, flags);
	if (open)
	{
		if (not title.empty())
		{
			ImGui::PushFont(nullptr, style.FontSizeBase * metrics::font_modal_title);
			ImGui::TextUnformatted(title.c_str());
			ImGui::PopFont();
			ImGui::Dummy({0, style.ItemSpacing.y});
		}
	}
	else
	{
		ImGui::PopStyleVar(3);
		ImGui::PopStyleColor(3);
	}
	return open;
}

void end_modal()
{
	ImGui::EndPopup();
	ImGui::PopStyleVar(3);
	ImGui::PopStyleColor(3);
}

int confirm_modal(const char * id, const std::string & title, const std::string & message, const std::string & confirm_label, const std::string & cancel_label, bool danger)
{
	int result = 0;
	if (begin_modal(id, title, 520))
	{
		const theme & t = current();
		ImGui::PushStyleColor(ImGuiCol_Text, t.text_muted);
		ImGui::TextWrapped("%s", message.c_str());
		ImGui::PopStyleColor();
		ImGui::Dummy({0, 12});

		const float gap = ImGui::GetStyle().ItemSpacing.x;
		const float cancel_w = ImGui::CalcTextSize(cancel_label.c_str()).x + metrics::button_padding.x * 2;
		const float confirm_w = ImGui::CalcTextSize(confirm_label.c_str()).x + metrics::button_padding.x * 2;
		ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - cancel_w - confirm_w - gap);
		if (button(cancel_label, button_style::secondary, {cancel_w, 0}))
		{
			result = -1;
			ImGui::CloseCurrentPopup();
		}
		ImGui::SameLine();
		if (button(confirm_label, danger ? button_style::danger : button_style::primary, {confirm_w, 0}))
		{
			result = 1;
			ImGui::CloseCurrentPopup();
		}
		end_modal();
	}
	return result;
}

namespace
{
std::vector<ImVec2> list_row_after; // cursor to restore at end_list_row
}

list_row_result begin_list_row(const char * id, const char * icon, ImTextureID image, const std::string & title, const std::string & subtitle, bool selected, float trailing_width, float height, bool large_thumb, bool interactive)
{
	ImGuiWindow * window = ImGui::GetCurrentWindow();
	const theme & t = current();
	const ImGuiStyle & style = ImGui::GetStyle();

	// measure title + (possibly multi-line) subtitle so the row can auto-size
	const float pad = metrics::list_row_pad;
	const float box = large_thumb ? metrics::list_row_box_large : metrics::list_row_box;
	const ImVec2 ts = ImGui::CalcTextSize(title.c_str());
	ImVec2 sub_sz{0, 0};
	if (not subtitle.empty())
	{
		ImGui::PushFont(nullptr, style.FontSizeBase * metrics::font_description);
		sub_sz = ImGui::CalcTextSize(subtitle.c_str());
		ImGui::PopFont();
	}
	const float text_h = ts.y + (subtitle.empty() ? 0 : metrics::label_line_gap + sub_sz.y);
	const float row_h = height > 0 ? height : ImMax(box, text_h) + 2 * pad;
	const ImVec2 p0 = window->DC.CursorPos;
	const float avail = ImGui::GetContentRegionAvail().x;

	// non-interactive rows just reserve layout space, no click area and no hover highlight
	bool clicked = false;
	if (interactive)
	{
		// the click area excludes the trailing controls so they receive their own clicks
		ImGui::SetNextItemAllowOverlap();
		clicked = ImGui::InvisibleButton(id, {ImMax(avail - trailing_width, 1.f), row_h});
		hover_haptic();
	}
	else
		ImGui::Dummy({avail, row_h});
	const bool hovered = interactive and ImGui::IsItemHovered();
	list_row_after.push_back(window->DC.CursorPos);

	ImDrawList * draw = window->DrawList;
	const ImRect bb(p0, p0 + ImVec2(avail, row_h));
	// no tinted background for the selected row, callers mark the active item with a chip
	if (hovered)
		draw->AddRectFilled(bb.Min, bb.Max, t.col(t.control), t.card_rounding);

	// leading thumbnail or icon box, large_thumb spans the full row height
	const float thumb = large_thumb ? row_h - 2 * pad : box;
	const ImVec2 box_min = {p0.x + pad, p0.y + (row_h - thumb) * 0.5f};
	const ImVec2 box_max = box_min + ImVec2(thumb, thumb);
	if (image)
		draw->AddImageRounded(image, box_min, box_max, {0, 0}, {1, 1}, IM_COL32_WHITE, t.rounding);
	else
	{
		draw->AddRectFilled(box_min, box_max, t.col(selected ? t.accent : t.control), t.rounding);
		if (icon and icon[0])
		{
			const ImVec2 is = ImGui::CalcTextSize(icon);
			draw->AddText(box_min + (ImVec2(thumb, thumb) - is) * 0.5f, t.col(selected ? t.on_accent : t.text_muted), icon);
		}
	}

	// title + subtitle
	const float tx = box_max.x + pad;
	const float total = subtitle.empty() ? ts.y : ts.y + metrics::label_line_gap + sub_sz.y;
	const float ty = p0.y + (row_h - total) * 0.5f;
	draw->AddText({tx, ty}, t.col(t.text), title.c_str());
	if (not subtitle.empty())
	{
		ImGui::PushFont(nullptr, style.FontSizeBase * metrics::font_description);
		draw->AddText({tx, ty + ts.y + metrics::label_line_gap}, t.col(t.text_muted), subtitle.c_str());
		ImGui::PopFont();
	}

	// trailing controls right-align against the inner padding
	return {bb.Min, {bb.Max.x - pad, bb.Max.y}, clicked};
}

void end_list_row()
{
	ImGui::GetCurrentWindow()->DC.CursorPos = list_row_after.back();
	list_row_after.pop_back();
}

int action_menu(const char * id, const char * icon, const std::vector<action_item> & items)
{
	const theme & t = current();

	if (icon_button(icon, {}, false))
		ImGui::OpenPopup(id);

	int chosen = -1;

	const float pad = metrics::combo_padding.x;
	const float icon_w = ImGui::GetFontSize() * 1.5f;
	float content_w = 0;
	for (const auto & it: items)
		content_w = ImMax(content_w, icon_w + pad + ImGui::CalcTextSize(it.label).x + pad * 2 + ImGui::GetFontSize());

	// centred modal on the popup layer, like combo()
	ImGui::SetNextWindowPos(popup_center, ImGuiCond_Always, {0.5f, 0.5f});
	ImGui::SetNextWindowSize({ImMax(content_w, metrics::combo_modal_min_width), 0});

	ImGui::PushStyleColor(ImGuiCol_PopupBg, t.card);
	ImGui::PushStyleColor(ImGuiCol_Border, t.border);
	ImGui::PushStyleColor(ImGuiCol_Text, t.text);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, t.card_rounding);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, metrics::card_padding);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, t.border_size > 0 ? t.border_size : 1);

	if (ImGui::BeginPopupModal(id, nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_AlwaysAutoResize))
	{
		ImGuiWindow * window = ImGui::GetCurrentWindow();
		const float row_h = ImGui::GetFrameHeight() * metrics::control_height;

		for (int i = 0; i < int(items.size()); ++i)
		{
			const action_item & it = items[i];
			ImGui::PushID(i);
			const float row_w = ImGui::GetContentRegionAvail().x;
			const ImVec2 pos = window->DC.CursorPos;
			const ImRect bb(pos, pos + ImVec2(row_w, row_h));
			ImGui::ItemSize(bb, 0);
			const ImGuiID gid = window->GetID("##row");
			if (ImGui::ItemAdd(bb, gid))
			{
				bool hovered, held;
				if (ImGui::ButtonBehavior(bb, gid, &hovered, &held))
				{
					chosen = i;
					ImGui::CloseCurrentPopup();
				}
				hover_haptic();

				ImDrawList * draw = window->DrawList;
				if (hovered)
					draw->AddRectFilled(bb.Min, bb.Max, t.col(t.control_hovered), t.rounding);

				const ImU32 fg = t.col(it.danger ? t.danger : t.text);
				const ImVec2 its = ImGui::CalcTextSize(it.icon);
				draw->AddText({bb.Min.x + pad + (icon_w - its.x) * 0.5f, bb.Min.y + (row_h - its.y) * 0.5f}, fg, it.icon);
				const ImVec2 ls = ImGui::CalcTextSize(it.label);
				draw->AddText({bb.Min.x + pad + icon_w, bb.Min.y + (row_h - ls.y) * 0.5f}, fg, it.label);
				if (it.checked)
				{
					const ImVec2 cs = ImGui::CalcTextSize(ICON_FA_CHECK);
					draw->AddText({bb.Max.x - pad - cs.x, bb.Min.y + (row_h - cs.y) * 0.5f}, t.col(t.accent), ICON_FA_CHECK);
				}
			}
			ImGui::PopID();
		}

		// click outside dismisses, AllowWhenBlockedByActiveItem so pressing a row still counts as inside
		if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) and not ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows | ImGuiHoveredFlags_AllowWhenBlockedByActiveItem))
			ImGui::CloseCurrentPopup();

		ImGui::EndPopup();
	}

	ImGui::PopStyleVar(3);
	ImGui::PopStyleColor(3);
	return chosen;
}

void top_bar(float height, ImTextureID logo, const std::vector<top_bar_item> & right_items)
{
	const float side = ImGui::GetFrameHeight() * metrics::control_height;
	const float gap = 8;
	const float margin = 24;

	// transparent child bg so the alpha-blended window background shows through
	ImGui::SetCursorPos({0, 0});
	ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4{0, 0, 0, 0});
	ImGui::BeginChild("TopBar", {ImGui::GetWindowSize().x, height}, 0, ImGuiWindowFlags_NoScrollbar);
	{
		// A low-contrast top wash separates navigation from content while the
		// cyan rule gives the shell a crisp Hyprland-like edge.
		const theme & t = current();
		ImDrawList * draw = ImGui::GetWindowDrawList();
		const ImVec2 top = ImGui::GetWindowPos();
		const ImVec2 right = top + ImVec2{ImGui::GetWindowSize().x, height};
		ImVec4 wash = t.card;
		wash.w = background_alpha() * 0.72f;
		draw->AddRectFilledMultiColor(top, right, t.col(wash), t.col(wash), t.col({wash.x, wash.y, wash.z, wash.w * 0.35f}), t.col({wash.x, wash.y, wash.z, wash.w * 0.35f}));
		draw->AddRectFilled({top.x + margin, right.y - 2}, {top.x + margin + 76, right.y}, t.col(t.accent), 1.f);
		draw->AddLine({top.x, right.y - 1}, {right.x, right.y - 1}, t.col(t.border));

		// logo: WiVRn mascot followed by the wordmark
		const float logo_size = 44;
		ImGui::SetCursorPos({margin, (height - logo_size) * 0.5f});
		if (logo)
			ImGui::Image(logo, {logo_size, logo_size});
		ImGui::SameLine(0, 12);
		ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 1.3f);
		ImGui::SetCursorPosY((height - ImGui::GetFontSize()) * 0.5f);
		ImGui::TextUnformatted("WiVRn");
		ImGui::PopFont();

		// right-aligned cluster
		float cluster_w = 0;
		for (const auto & it: right_items)
			cluster_w += it.width;
		if (not right_items.empty())
			cluster_w += gap * (right_items.size() - 1);

		ImGui::SetCursorPos({ImGui::GetWindowSize().x - margin - cluster_w, (height - side) * 0.5f});
		for (size_t i = 0; i < right_items.size(); ++i)
		{
			ImGui::SetCursorPosY((height - side) * 0.5f);
			right_items[i].draw();
			if (i + 1 < right_items.size())
				ImGui::SameLine(0, gap);
		}
	}
	ImGui::EndChild();
	ImGui::PopStyleColor();
}

void begin_sidebar(float top_bar_h, float tab_width, int footer_items)
{
	// shares the transparent window background, sections separated by shell_dividers
	ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4{0, 0, 0, 0});
	ImGui::SetCursorPos({0, top_bar_h});
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {16, 16});
	ImGui::BeginChild("Tabs", {tab_width, ImGui::GetWindowSize().y - top_bar_h}, ImGuiChildFlags_AlwaysUseWindowPadding);
	ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {0, 4});

	// nav list scrolls in a child that fills the rest, footer stays pinned in footer_items rows
	const float row_h = ImGui::GetFrameHeight() * metrics::control_height + ImGui::GetStyle().ItemSpacing.y;
	sidebar_footer_height = footer_items > 0 ? footer_items * row_h : 0;
	ImGui::BeginChild("TabsScroll", {0, ImGui::GetContentRegionAvail().y - sidebar_footer_height});
}

void sidebar_footer()
{
	::ScrollWhenDragging(); // drag to scroll the nav list, like the rest of the GUI
	ImGui::EndChild();      // TabsScroll
	// pin the footer to the bottom of the sidebar
	ImGui::SetCursorPosY(ImGui::GetContentRegionMax().y - sidebar_footer_height);
}

void end_sidebar()
{
	ImGui::PopStyleVar();   // ImGuiStyleVar_ItemSpacing
	ImGui::EndChild();      // Tabs
	ImGui::PopStyleVar();   // ImGuiStyleVar_WindowPadding
	ImGui::PopStyleColor(); // ImGuiCol_ChildBg
}

void shell_dividers(float top_bar_h, float tab_width)
{
	ImVec4 divider_col = current().border;
	divider_col.w *= background_alpha(); // follow the panel opacity
	const ImU32 divider = ImGui::GetColorU32(divider_col);
	const ImVec2 win_pos = ImGui::GetWindowPos();
	const ImVec2 win_size = ImGui::GetWindowSize();
	ImDrawList * dl = ImGui::GetWindowDrawList();
	dl->AddLine({win_pos.x, win_pos.y + top_bar_h}, {win_pos.x + win_size.x, win_pos.y + top_bar_h}, divider);
	dl->AddLine({win_pos.x + tab_width, win_pos.y + top_bar_h}, {win_pos.x + tab_width, win_pos.y + win_size.y}, divider);
	ImVec4 glow = current().accent;
	glow.w = 0.20f * background_alpha();
	dl->AddLine({win_pos.x + tab_width + 1, win_pos.y + top_bar_h + 8}, {win_pos.x + tab_width + 1, win_pos.y + win_size.y - 8}, current().col(glow));
}

} // namespace wivrn::ui
