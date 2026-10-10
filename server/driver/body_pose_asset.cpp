/*
 * WiVRn VR streaming
 * Copyright (C) 2026 WiVRn-fsr contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "body_pose_asset.h"
#include "wivrn_config.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <stdexcept>
#include <string_view>

namespace wivrn
{
namespace
{
constexpr float epsilon = 1.0e-6f;
constexpr size_t maximum_pose_bytes = 256 * 1024;
constexpr size_t maximum_pose_files = 64;
constexpr std::string_view animation_25_uuid = "unity.9b8a991000738a04d984a41268587e5a";

xrt_vec3 add(const xrt_vec3 & a, const xrt_vec3 & b)
{
	return {a.x + b.x, a.y + b.y, a.z + b.z};
}

xrt_vec3 sub(const xrt_vec3 & a, const xrt_vec3 & b)
{
	return {a.x - b.x, a.y - b.y, a.z - b.z};
}

xrt_vec3 mul(const xrt_vec3 & value, float factor)
{
	return {value.x * factor, value.y * factor, value.z * factor};
}

float dot(const xrt_vec3 & a, const xrt_vec3 & b)
{
	return a.x * b.x + a.y * b.y + a.z * b.z;
}

xrt_vec3 cross(const xrt_vec3 & a, const xrt_vec3 & b)
{
	return {
	        a.y * b.z - a.z * b.y,
	        a.z * b.x - a.x * b.z,
	        a.x * b.y - a.y * b.x,
	};
}

float length(const xrt_vec3 & value)
{
	return std::sqrt(dot(value, value));
}

xrt_vec3 normalize(const xrt_vec3 & value, xrt_vec3 fallback = {0.0f, 0.0f, 1.0f})
{
	const float norm = length(value);
	return norm > epsilon ? mul(value, 1.0f / norm) : fallback;
}

xrt_quat normalize(const xrt_quat & value)
{
	const float norm = std::sqrt(value.x * value.x + value.y * value.y + value.z * value.z + value.w * value.w);
	return norm > epsilon ? xrt_quat{value.x / norm, value.y / norm, value.z / norm, value.w / norm}
	                      : xrt_quat{0.0f, 0.0f, 0.0f, 1.0f};
}

xrt_quat multiply(const xrt_quat & a, const xrt_quat & b)
{
	return {
	        a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
	        a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
	        a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
	        a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z,
	};
}

xrt_quat nlerp(const xrt_quat & from, const xrt_quat & to, float amount)
{
	xrt_quat target = to;
	if (from.x * to.x + from.y * to.y + from.z * to.z + from.w * to.w < 0.0f)
		target = {-to.x, -to.y, -to.z, -to.w};
	return normalize(xrt_quat{
	        from.x + (target.x - from.x) * amount,
	        from.y + (target.y - from.y) * amount,
	        from.z + (target.z - from.z) * amount,
	        from.w + (target.w - from.w) * amount,
	});
}

xrt_vec3 rotate(const xrt_quat & orientation, const xrt_vec3 & value)
{
	const xrt_vec3 q{orientation.x, orientation.y, orientation.z};
	const auto t = mul(cross(q, value), 2.0f);
	return add(add(value, mul(t, orientation.w)), cross(q, t));
}

xrt_quat from_to(const xrt_vec3 & from, const xrt_vec3 & to)
{
	const auto source = normalize(from, {0.0f, 1.0f, 0.0f});
	const auto target = normalize(to, source);
	const float cosine = std::clamp(dot(source, target), -1.0f, 1.0f);
	if (cosine > 1.0f - epsilon)
		return {0.0f, 0.0f, 0.0f, 1.0f};
	if (cosine < -1.0f + epsilon)
	{
		auto axis = normalize(cross(source, {1.0f, 0.0f, 0.0f}), {0.0f, 0.0f, 1.0f});
		if (length(axis) <= epsilon)
			axis = normalize(cross(source, {0.0f, 1.0f, 0.0f}), {0.0f, 0.0f, 1.0f});
		return {axis.x, axis.y, axis.z, 0.0f};
	}
	const auto axis = cross(source, target);
	return normalize(xrt_quat{axis.x, axis.y, axis.z, 1.0f + cosine});
}

xrt_quat tracker_orientation(const xrt_vec3 & anatomical_up, const xrt_vec3 & anatomical_forward)
{
	const auto up = normalize(anatomical_up, {0.0f, 1.0f, 0.0f});
	const auto fallback_forward = normalize(cross({1.0f, 0.0f, 0.0f}, up), {0.0f, 0.0f, 1.0f});
	const auto projected_forward = normalize(sub(anatomical_forward, mul(up, dot(anatomical_forward, up))),
	                                         fallback_forward);
	const auto swing = from_to({0.0f, 1.0f, 0.0f}, up);
	const auto swung_forward = rotate(swing, {0.0f, 0.0f, 1.0f});
	return normalize(multiply(from_to(swung_forward, projected_forward), swing));
}

bool finite(const xrt_vec3 & value)
{
	return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

bool finite(const xrt_quat & value)
{
	return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z) && std::isfinite(value.w);
}

xrt_vec3 constrain_endpoint(const xrt_vec3 & root, const xrt_vec3 & endpoint, float minimum, float maximum)
{
	const auto delta = sub(endpoint, root);
	return add(root, mul(normalize(delta), std::clamp(length(delta), minimum, maximum)));
}

xrt_vec3 solve_knee(const xrt_vec3 & hip,
                    const xrt_vec3 & ankle,
                    const xrt_vec3 & pole,
                    float thigh,
                    float shin)
{
	const auto hip_to_ankle = sub(ankle, hip);
	const float distance = std::clamp(length(hip_to_ankle), 0.04f, thigh + shin - 0.001f);
	const auto direction = normalize(hip_to_ankle);
	const float along = (thigh * thigh - shin * shin + distance * distance) / (2.0f * distance);
	const float radius = std::sqrt(std::max(0.0f, thigh * thigh - along * along));
	const auto pole_delta = sub(pole, hip);
	auto bend = sub(pole_delta, mul(direction, dot(pole_delta, direction)));
	if (length(bend) <= epsilon)
		bend = cross(direction, {1.0f, 0.0f, 0.0f});
	if (length(bend) <= epsilon)
		bend = cross(direction, {0.0f, 1.0f, 0.0f});
	return add(add(hip, mul(direction, along)), mul(normalize(bend), radius));
}

xrt_vec3 foot_endpoint(const xrt_vec3 & ankle,
                       const xrt_vec3 & direction,
                       float foot_length,
                       float surface_y,
                       const xrt_vec3 & fallback_forward)
{
	const auto candidate = add(ankle, mul(direction, foot_length));
	if (candidate.y >= surface_y)
		return candidate;
	const float vertical = surface_y - ankle.y;
	const float planar_length = std::sqrt(std::max(0.0f, foot_length * foot_length - vertical * vertical));
	const auto planar_direction = normalize({direction.x, 0.0f, direction.z}, fallback_forward);
	return add(add(ankle, mul(planar_direction, planar_length)), {0.0f, vertical, 0.0f});
}

xrt_vec3 read_vec3(const nlohmann::json & value, std::string_view field)
{
	if (!value.is_array() || value.size() != 3)
		throw std::runtime_error(std::string(field) + " must contain exactly three numbers");
	xrt_vec3 result{value[0].get<float>(), value[1].get<float>(), value[2].get<float>()};
	if (!finite(result))
		throw std::runtime_error(std::string(field) + " contains a non-finite value");
	return result;
}

body_pose_kind read_category(const std::string & value)
{
	if (value == "back")
		return body_pose_kind::back;
	if (value == "front")
		return body_pose_kind::front;
	if (value == "left_side")
		return body_pose_kind::left_side;
	if (value == "right_side")
		return body_pose_kind::right_side;
	throw std::runtime_error("unsupported lying-pose category: " + value);
}

void validate_direction(const xrt_vec3 & value, std::string_view field)
{
	const float norm = length(value);
	if (norm < 0.85f || norm > 1.15f)
		throw std::runtime_error(std::string(field) + " must be normalized");
}

void validate_point(const xrt_vec3 & value, std::string_view field)
{
	if (std::abs(value.x) > 2.0f || std::abs(value.y) > 2.0f || std::abs(value.z) > 2.0f)
		throw std::runtime_error(std::string(field) + " exceeds the normalized pose bounds");
}

body_pose_asset read_asset(const std::filesystem::path & path)
{
	const auto size = std::filesystem::file_size(path);
	if (size == 0 || size > maximum_pose_bytes)
		throw std::runtime_error("pose file size is outside the accepted range");
	const auto json = nlohmann::json::parse(std::ifstream(path));
	if (json.at("format") != "wivrn-pose" || json.at("version") != 1 || json.at("units") != "height_ratio")
		throw std::runtime_error("unsupported pose format, version or units");

	body_pose_asset asset{};
	asset.uuid = json.at("uuid").get<std::string>();
	asset.name = json.at("name").get<std::string>();
	asset.category = read_category(json.at("category").get<std::string>());
	asset.reference_height_m = json.at("reference_height_m").get<float>();
	if (asset.uuid.empty() || asset.uuid.size() > 96 || asset.name.empty() || asset.name.size() > 96)
		throw std::runtime_error("pose UUID or name is outside the accepted bounds");
	if (!std::isfinite(asset.reference_height_m) || asset.reference_height_m < 1.0f || asset.reference_height_m > 2.5f)
		throw std::runtime_error("reference_height_m must be between 1.0 and 2.5");

	const auto & root = json.at("root");
	if (root.at("mode") != "hybrid")
		throw std::runtime_error("the integrated solver currently requires hybrid root mode");
	asset.root.head = read_vec3(root.at("head"), "root.head");
	asset.root.hip = read_vec3(root.at("hip"), "root.hip");
	asset.root.chest = read_vec3(root.at("chest"), "root.chest");
	asset.root.chest_forward = read_vec3(root.at("chest_forward"), "root.chest_forward");
	asset.root.hip_forward = read_vec3(root.at("hip_forward"), "root.hip_forward");
	asset.root.head_surface_blend = root.at("head_surface_blend").get<float>();
	if (!std::isfinite(asset.root.head_surface_blend) || asset.root.head_surface_blend < 0.0f ||
	    asset.root.head_surface_blend > 1.0f)
		throw std::runtime_error("root.head_surface_blend must be between 0 and 1");
	validate_point(asset.root.head, "root.head");
	validate_point(asset.root.hip, "root.hip");
	validate_point(asset.root.chest, "root.chest");
	validate_direction(asset.root.chest_forward, "root.chest_forward");
	validate_direction(asset.root.hip_forward, "root.hip_forward");

	const auto & legs = json.at("legs");
	for (size_t side = 0; side < 2; ++side)
	{
		const auto & leg = legs.at(side == 0 ? "left" : "right");
		asset.legs[side].ankle = read_vec3(leg.at("ankle"), "leg.ankle");
		asset.legs[side].foot_direction = read_vec3(leg.at("foot_direction"), "leg.foot_direction");
		asset.legs[side].bend_pole = read_vec3(leg.at("bend_pole"), "leg.bend_pole");
		asset.legs[side].contact_enabled = leg.at("contact").at("enabled").get<bool>();
		validate_point(asset.legs[side].ankle, "leg.ankle");
		validate_point(asset.legs[side].bend_pole, "leg.bend_pole");
		validate_direction(asset.legs[side].foot_direction, "leg.foot_direction");
		if (asset.legs[side].contact_enabled)
			throw std::runtime_error("persistent asset contacts are not enabled in the experimental integration");
	}
	return asset;
}

std::vector<std::filesystem::path> pose_roots()
{
	std::vector<std::filesystem::path> result;
	// Portable distributions expose their relocated data through XDG_DATA_DIRS.
	if (const char * data_dirs = std::getenv("XDG_DATA_DIRS"); data_dirs && *data_dirs)
	{
		std::string dirs(data_dirs);
		size_t start = 0;
		while (start < dirs.size())
		{
			const auto end = dirs.find(':', start);
			const auto dir = dirs.substr(start, end - start);
			if (!dir.empty() && std::filesystem::path(dir).is_absolute())
				result.emplace_back(std::filesystem::path(dir) / "wivrn" / "poses");
			if (end == std::string::npos)
				break;
			start = end + 1;
		}
	}
#ifdef WIVRN_BODY_POSE_SOURCE_DIR
	result.emplace_back(WIVRN_BODY_POSE_SOURCE_DIR);
#endif
#ifdef WIVRN_INSTALL_PREFIX
	result.emplace_back(WIVRN_INSTALL_PREFIX "/share/wivrn/poses");
#endif
	if (const char * data_home = std::getenv("XDG_DATA_HOME"); data_home && *data_home)
		result.emplace_back(std::filesystem::path(data_home) / "wivrn" / "poses");
	else if (const char * home = std::getenv("HOME"); home && *home)
		result.emplace_back(std::filesystem::path(home) / ".local" / "share" / "wivrn" / "poses");
	return result;
}

class catalog
{
public:
	catalog()
	{
		std::set<std::filesystem::path> visited;
		size_t files = 0;
		for (const auto & root: pose_roots())
		{
			std::error_code error;
			if (!std::filesystem::is_directory(root, error))
				continue;
			for (std::filesystem::recursive_directory_iterator iterator(root, error), end;
			     !error && iterator != end && files < maximum_pose_files;
			     iterator.increment(error))
			{
				if (!iterator->is_regular_file(error) || !iterator->path().string().ends_with(".wivrnpose.json"))
					continue;
				auto canonical = std::filesystem::weakly_canonical(iterator->path(), error);
				if (error || !visited.insert(canonical).second)
					continue;
				++files;
				try
				{
					auto asset = read_asset(canonical);
					assets_[asset.uuid] = std::move(asset);
				}
				catch (const std::exception & exception)
				{
					++metrics_.rejected;
					metrics_.issues.push_back(canonical.string() + ": " + exception.what());
				}
			}
			if (error)
				metrics_.issues.push_back(root.string() + ": " + error.message());
		}
		metrics_.loaded = assets_.size();
	}

	const body_pose_asset * find(body_pose_asset_selection selection, body_pose_kind category) const
	{
		std::string_view uuid;
		switch (selection)
		{
			case body_pose_asset_selection::procedural:
				return nullptr;
			case body_pose_asset_selection::unity_animation_25:
				uuid = animation_25_uuid;
				break;
		}
		const auto iterator = assets_.find(std::string(uuid));
		return iterator != assets_.end() && iterator->second.category == category ? &iterator->second : nullptr;
	}

	std::map<std::string, body_pose_asset> assets_;
	body_pose_catalog_metrics metrics_{};
};

const catalog & shared_catalog()
{
	static const catalog value;
	return value;
}
} // namespace

const body_pose_asset * find_body_pose_asset(body_pose_asset_selection selection, body_pose_kind category)
{
	return shared_catalog().find(selection, category);
}

const body_pose_catalog_metrics & body_pose_catalog_status()
{
	return shared_catalog().metrics_;
}

body_pose_asset_solution solve_body_pose_asset(const body_pose_asset & asset,
                                               const body_pose_asset_solve_input & input)
{
	body_pose_asset_solution result{};
	if (!finite(input.head) || !finite(input.body_forward) || !std::isfinite(input.surface_y) ||
	    !std::isfinite(input.body_height_m) || input.body_height_m < 1.0f || input.body_height_m > 2.5f)
		return result;

	const xrt_vec3 world_up{0.0f, 1.0f, 0.0f};
	auto forward = normalize(xrt_vec3{input.body_forward.x, 0.0f, input.body_forward.z});
	auto right = normalize(cross(world_up, forward), {1.0f, 0.0f, 0.0f});
	forward = normalize(cross(right, world_up), forward);
	const float scale = input.body_height_m;
	const float influence = std::clamp(input.target_influence, 0.0f, 1.0f);
	const auto blend_point = [influence](const xrt_vec3 & source, const xrt_vec3 & target, bool source_valid) {
		return source_valid ? add(mul(source, 1.0f - influence), mul(target, influence)) : target;
	};
	const auto point = [&](const xrt_vec3 & local, const xrt_vec3 & origin) {
		return add(add(add(origin, mul(right, local.x * scale)), mul(world_up, local.y * scale)),
		           mul(forward, local.z * scale));
	};
	const auto direction = [&](const xrt_vec3 & local) {
		return normalize(add(add(mul(right, local.x), mul(world_up, local.y)), mul(forward, local.z)));
	};

	xrt_vec3 origin = sub(sub(input.head, mul(right, asset.root.head.x * scale)),
	                      mul(forward, asset.root.head.z * scale));
	const float head_locked_origin_y = input.head.y - asset.root.head.y * scale;
	origin.y = input.surface_y * (1.0f - asset.root.head_surface_blend) +
	           head_locked_origin_y * asset.root.head_surface_blend;

	const float torso_length = input.skeleton.valid
	                                   ? std::clamp(input.skeleton.torso_length, 0.34f, 0.62f)
	                                   : std::clamp(scale * 0.27f, 0.38f, 0.52f);
	const float pelvis_half_width = input.skeleton.valid
	                                        ? std::clamp(input.skeleton.pelvis_half_width, 0.065f, 0.14f)
	                                        : std::clamp(scale * 0.052f, 0.075f, 0.11f);
	const auto authored_hip = blend_point(input.source_hip, point(asset.root.hip, origin), input.source_hip_valid);
	result.hip = constrain_endpoint(input.head,
	                                authored_hip,
	                                std::max(0.30f, torso_length * 0.85f),
	                                std::min(0.68f, torso_length * 1.15f));
	result.chest = blend_point(input.source_chest, point(asset.root.chest, origin), input.source_chest_valid);
	const float chest_limit = length(sub(input.head, result.hip)) * 0.75f;
	result.chest = constrain_endpoint(input.head, result.chest, 0.08f, std::max(0.09f, chest_limit));
	result.hip_joints = std::array<xrt_vec3, 2>{
	        sub(result.hip, mul(right, pelvis_half_width)),
	        add(result.hip, mul(right, pelvis_half_width)),
	};

	for (size_t side = 0; side < 2; ++side)
	{
		const float thigh = input.skeleton.valid
		                            ? std::clamp(input.skeleton.thigh_length[side], 0.28f, 0.58f)
		                            : std::clamp(scale * 0.245f, 0.32f, 0.52f);
		const float shin = input.skeleton.valid
		                           ? std::clamp(input.skeleton.shin_length[side], 0.28f, 0.58f)
		                           : std::clamp(scale * 0.246f, 0.32f, 0.52f);
		const float foot = input.skeleton.valid
		                           ? std::clamp(input.skeleton.ankle_to_foot[side], 0.035f, 0.16f)
		                           : 0.08f;
		auto ankle = blend_point(input.source_ankles[side],
		                         point(asset.legs[side].ankle, origin),
		                         input.source_ankles_valid[side]);
		ankle.y = std::max(ankle.y, input.surface_y);
		ankle = constrain_endpoint(result.hip_joints[side],
		                           ankle,
		                           std::abs(thigh - shin) + 0.03f,
		                           thigh + shin - 0.003f);
		if (ankle.y < input.surface_y)
		{
			ankle.y = input.surface_y;
			ankle = constrain_endpoint(result.hip_joints[side],
			                           ankle,
			                           std::abs(thigh - shin) + 0.03f,
			                           thigh + shin - 0.003f);
		}
		const auto pole = blend_point(input.source_knees[side],
		                              point(asset.legs[side].bend_pole, origin),
		                              input.source_knees_valid[side]);
		result.ankles[side] = ankle;
		result.knees[side] = solve_knee(result.hip_joints[side], ankle, pole, thigh, shin);
		result.feet[side] = foot_endpoint(ankle,
		                                  direction(asset.legs[side].foot_direction),
		                                  foot,
		                                  input.surface_y,
		                                  forward);
		result.knee_orientation[side] = from_to({0.0f, -1.0f, 0.0f}, sub(ankle, result.knees[side]));
		result.foot_orientation[side] = from_to({0.0f, 0.0f, -1.0f}, sub(result.feet[side], ankle));
	}

	const auto torso_up = sub(result.chest, result.hip);
	result.chest_orientation = tracker_orientation(torso_up, direction(asset.root.chest_forward));
	result.hip_orientation = tracker_orientation(torso_up, direction(asset.root.hip_forward));
	if (input.source_orientations_valid[0])
		result.chest_orientation = nlerp(input.source_orientations[0], result.chest_orientation, influence);
	if (input.source_orientations_valid[1])
		result.hip_orientation = nlerp(input.source_orientations[1], result.hip_orientation, influence);
	for (size_t side = 0; side < 2; ++side)
	{
		if (input.source_orientations_valid[2 + side])
			result.knee_orientation[side] =
			        nlerp(input.source_orientations[2 + side], result.knee_orientation[side], influence);
		if (input.source_orientations_valid[4 + side])
			result.foot_orientation[side] =
			        nlerp(input.source_orientations[4 + side], result.foot_orientation[side], influence);
	}
	result.valid = finite(result.chest) && finite(result.hip) && finite(result.chest_orientation) &&
	               finite(result.hip_orientation);
	for (size_t side = 0; side < 2; ++side)
		result.valid = result.valid && finite(result.hip_joints[side]) && finite(result.knees[side]) &&
		               finite(result.ankles[side]) && finite(result.feet[side]) &&
		               finite(result.knee_orientation[side]) && finite(result.foot_orientation[side]);
	return result;
}

} // namespace wivrn
