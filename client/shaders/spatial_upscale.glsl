/*
 * WiVRn headset spatial reconstruction.
 *
 * SGSR1 adaptation:
 * Copyright (c) 2025 Qualcomm Innovation Center, Inc.
 * SPDX-License-Identifier: BSD-3-Clause
 * Based on Snapdragon Game Super Resolution 1's Vulkan GLSL shader.
 *
 * The NIS path follows the directional scaling and adaptive sharpening
 * formulation published in NVIDIA Image Scaling SDK 1.0.3.
 * Copyright (c) 2022 NVIDIA CORPORATION & AFFILIATES.
 * SPDX-License-Identifier: MIT
 *
 * The FSR path is a GLSL adaptation of AMD FidelityFX FSR 1 EASU/RCAS.
 * Copyright (c) 2021 Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: MIT
 */

#version 450

#ifdef VERT_SHADER

layout(location = 0) out vec2 out_uv;

void main()
{
	const vec2 positions[3] = vec2[3](vec2(-1.0, -1.0), vec2(3.0, -1.0), vec2(-1.0, 3.0));
	vec2 p = positions[gl_VertexIndex];
	gl_Position = vec4(p, 0.0, 1.0);
	out_uv = p * 0.5 + 0.5;
}

#endif

#ifdef FRAG_SHADER

// 0 bilinear, 1 SGSR1, 2 FSR1 EASU, 4 FSR1 RCAS, 5 NIS output copy
layout(constant_id = 0) const int algorithm = 0;

layout(set = 0, binding = 0) uniform sampler2D source_color;

layout(push_constant) uniform constants
{
	vec4 viewport; // inverse input size, then input size
	vec4 output_viewport; // inverse output size, then output size
	float sharpness;
	uint premultiply_alpha;
} pc;

layout(location = 0) in vec2 in_uv;
layout(location = 0) out vec4 out_color;

vec3 linear_to_srgb(vec3 color)
{
	vec3 low = color * 12.92;
	vec3 high = 1.055 * pow(max(color, vec3(0.0)), vec3(1.0 / 2.4)) - 0.055;
	return mix(high, low, lessThanEqual(color, vec3(0.0031308)));
}

vec4 linear_to_srgb(vec4 channel)
{
	vec4 low = channel * 12.92;
	vec4 high = 1.055 * pow(max(channel, vec4(0.0)), vec4(1.0 / 2.4)) - 0.055;
	return mix(high, low, lessThanEqual(channel, vec4(0.0031308)));
}

vec3 srgb_to_linear(vec3 color)
{
	vec3 low = color / 12.92;
	vec3 high = pow((max(color, vec3(0.0)) + 0.055) / 1.055, vec3(2.4));
	return mix(high, low, lessThanEqual(color, vec3(0.04045)));
}

float fast_lanczos2(float x)
{
	float a = x - 4.0;
	float b = x * a - a;
	a *= a;
	return b * a;
}

vec2 sgsr_weight(float dx, float dy, float c, vec3 data)
{
	float edge_distance = dx * data.z + dy * data.y;
	float x = dx * dx + dy * dy + edge_distance * edge_distance *
	          (clamp(c * c * data.x, 0.0, 1.0) * 0.7 - 1.0);
	float w = fast_lanczos2(x);
	return vec2(w, w * c);
}

vec2 sgsr_edge_direction(vec4 left, vec4 right)
{
	float a = right.x - left.z;
	float b = right.w - left.y;
	vec2 delta = vec2(a + b, a - b);
	return delta * inversesqrt(dot(delta, delta) + 3.075740e-5);
}

vec4 sgsr1(vec2 uv)
{
	vec4 color = textureLod(source_color, uv, 0.0);
	color.rgb = linear_to_srgb(color.rgb);
	vec2 image_coord = uv * pc.viewport.zw + vec2(-0.5, 0.5);
	vec2 image_pixel = floor(image_coord);
	vec2 coord = image_pixel * pc.viewport.xy;
	vec2 phase = image_coord - image_pixel;
	vec4 left = linear_to_srgb(textureGather(source_color, coord, 1));
	float center_y = color.g;
	float edge_vote = abs(left.z - left.y) + abs(center_y - left.y) + abs(center_y - left.z);
	if (edge_vote <= 8.0 / 255.0)
	{
		color.rgb = srgb_to_linear(color.rgb);
		return color;
	}

	coord.x += pc.viewport.x;
	vec4 right = linear_to_srgb(textureGather(source_color, coord + vec2(pc.viewport.x, 0.0), 1));
	vec4 up_down;
	up_down.xy = linear_to_srgb(textureGather(source_color, coord + vec2(0.0, -pc.viewport.y), 1)).wz;
	up_down.zw = linear_to_srgb(textureGather(source_color, coord + vec2(0.0, pc.viewport.y), 1)).yx;
	float mean = (left.y + left.z + right.x + right.w) * 0.25;
	left -= vec4(mean);
	right -= vec4(mean);
	up_down -= vec4(mean);
	float centered = center_y - mean;
	float deviation = dot(abs(left), vec4(1.0)) + dot(abs(right), vec4(1.0)) +
	                  dot(abs(up_down), vec4(1.0));
	float inverse_deviation = 10.14185 / max(deviation, 1e-5);
	vec3 data = vec3(inverse_deviation * inverse_deviation, sgsr_edge_direction(left, right));
	vec2 weighted = sgsr_weight(phase.x, phase.y + 1.0, up_down.x, data);
	weighted += sgsr_weight(phase.x - 1.0, phase.y + 1.0, up_down.y, data);
	weighted += sgsr_weight(phase.x - 1.0, phase.y - 2.0, up_down.z, data);
	weighted += sgsr_weight(phase.x, phase.y - 2.0, up_down.w, data);
	weighted += sgsr_weight(phase.x + 1.0, phase.y - 1.0, left.x, data);
	weighted += sgsr_weight(phase.x, phase.y - 1.0, left.y, data);
	weighted += sgsr_weight(phase.x, phase.y, left.z, data);
	weighted += sgsr_weight(phase.x + 1.0, phase.y, left.w, data);
	weighted += sgsr_weight(phase.x - 1.0, phase.y - 1.0, right.x, data);
	weighted += sgsr_weight(phase.x - 2.0, phase.y - 1.0, right.y, data);
	weighted += sgsr_weight(phase.x - 2.0, phase.y, right.z, data);
	weighted += sgsr_weight(phase.x - 1.0, phase.y, right.w, data);
	float reconstructed = weighted.y / max(weighted.x, 1e-5);
	float minimum = min(min(left.y, left.z), min(right.x, right.w));
	float maximum = max(max(left.y, left.z), max(right.x, right.w));
	float delta = clamp(2.0 * reconstructed, minimum, maximum) - centered;
	delta = clamp(delta, -23.0 / 255.0, 23.0 / 255.0);
	color.rgb = clamp(color.rgb + delta, 0.0, 1.0);
	color.rgb = srgb_to_linear(color.rgb);
	return color;
}

void fsr_tap(inout vec3 color, inout float weight_sum, vec2 offset,
	         vec2 direction, vec2 length_scale, float lobe, float clip_point,
	         vec3 sample_color)
{
	vec2 v = vec2(offset.x * direction.x + offset.y * direction.y,
	              offset.x * -direction.y + offset.y * direction.x) * length_scale;
	float distance2 = min(dot(v, v), clip_point);
	float window_b = (0.4 * distance2 - 1.0);
	float window_a = (lobe * distance2 - 1.0);
	window_b *= window_b;
	window_a *= window_a;
	window_b = 25.0 / 16.0 * window_b - 9.0 / 16.0;
	float weight = window_b * window_a;
	color += sample_color * weight;
	weight_sum += weight;
}

void fsr_set(inout vec2 direction, inout float edge_length, vec2 phase,
	         vec4 quadrant, float a, float b, float c, float d, float e)
{
	float weight = dot(quadrant, vec4((1.0 - phase.x) * (1.0 - phase.y),
	                                  phase.x * (1.0 - phase.y),
	                                  (1.0 - phase.x) * phase.y,
	                                  phase.x * phase.y));
	float dc = d - c;
	float cb = c - b;
	float len_x = max(abs(dc), abs(cb));
	float dir_x = d - b;
	direction.x += dir_x * weight;
	len_x = len_x > 0.0 ? clamp(abs(dir_x) / len_x, 0.0, 1.0) : 0.0;
	edge_length += len_x * len_x * weight;
	float ec = e - c;
	float ca = c - a;
	float len_y = max(abs(ec), abs(ca));
	float dir_y = e - a;
	direction.y += dir_y * weight;
	len_y = len_y > 0.0 ? clamp(abs(dir_y) / len_y, 0.0, 1.0) : 0.0;
	edge_length += len_y * len_y * weight;
}

// AMD FidelityFX FSR1 Edge-Adaptive Spatial Upsampling (EASU), followed by
// the separate RCAS pass selected as algorithm 4 below.
vec4 fsr1(vec2 uv)
{
	vec2 output_pixel = floor(gl_FragCoord.xy);
	vec2 ratio = pc.viewport.zw / pc.output_viewport.zw;
	vec2 position = output_pixel * ratio + 0.5 * ratio - 0.5;
	vec2 base = floor(position);
	vec2 phase = position - base;
	vec2 p0 = base * pc.viewport.xy + vec2(pc.viewport.x, -pc.viewport.y);
	vec2 p1 = p0 + vec2(-pc.viewport.x, 2.0 * pc.viewport.y);
	vec2 p2 = p0 + vec2(pc.viewport.x, 2.0 * pc.viewport.y);
	vec2 p3 = p0 + vec2(0.0, 4.0 * pc.viewport.y);
	vec4 bc_r = linear_to_srgb(textureGather(source_color, p0, 0));
	vec4 bc_g = linear_to_srgb(textureGather(source_color, p0, 1));
	vec4 bc_b = linear_to_srgb(textureGather(source_color, p0, 2));
	vec4 ij_r = linear_to_srgb(textureGather(source_color, p1, 0));
	vec4 ij_g = linear_to_srgb(textureGather(source_color, p1, 1));
	vec4 ij_b = linear_to_srgb(textureGather(source_color, p1, 2));
	vec4 kl_r = linear_to_srgb(textureGather(source_color, p2, 0));
	vec4 kl_g = linear_to_srgb(textureGather(source_color, p2, 1));
	vec4 kl_b = linear_to_srgb(textureGather(source_color, p2, 2));
	vec4 on_r = linear_to_srgb(textureGather(source_color, p3, 0));
	vec4 on_g = linear_to_srgb(textureGather(source_color, p3, 1));
	vec4 on_b = linear_to_srgb(textureGather(source_color, p3, 2));
	vec4 bc_l = bc_b * 0.5 + bc_r * 0.5 + bc_g;
	vec4 ij_l = ij_b * 0.5 + ij_r * 0.5 + ij_g;
	vec4 kl_l = kl_b * 0.5 + kl_r * 0.5 + kl_g;
	vec4 on_l = on_b * 0.5 + on_r * 0.5 + on_g;
	vec2 direction = vec2(0.0);
	float edge_length = 0.0;
	fsr_set(direction, edge_length, phase, vec4(1, 0, 0, 0), bc_l.x, ij_l.w, ij_l.z, kl_l.w, ij_l.y);
	fsr_set(direction, edge_length, phase, vec4(0, 1, 0, 0), bc_l.y, ij_l.z, kl_l.w, kl_l.z, kl_l.x);
	fsr_set(direction, edge_length, phase, vec4(0, 0, 1, 0), ij_l.z, ij_l.x, ij_l.y, kl_l.x, on_l.w);
	fsr_set(direction, edge_length, phase, vec4(0, 0, 0, 1), kl_l.w, ij_l.y, kl_l.x, kl_l.y, on_l.z);
	float direction2 = dot(direction, direction);
	direction = direction2 < 1.0 / 32768.0 ? vec2(1.0, 0.0) : direction * inversesqrt(direction2);
	edge_length = 0.5 * edge_length;
	edge_length *= edge_length;
	float stretch = 1.0 / max(abs(direction.x), abs(direction.y));
	vec2 length_scale = vec2(1.0 + (stretch - 1.0) * edge_length, 1.0 - 0.5 * edge_length);
	float lobe = 0.5 + (0.21 - 0.5) * edge_length;
	float clip_point = 1.0 / lobe;
	vec3 minimum = min(min(vec3(ij_r.z, ij_g.z, ij_b.z), vec3(kl_r.w, kl_g.w, kl_b.w)),
	                   min(vec3(ij_r.y, ij_g.y, ij_b.y), vec3(kl_r.x, kl_g.x, kl_b.x)));
	vec3 maximum = max(max(vec3(ij_r.z, ij_g.z, ij_b.z), vec3(kl_r.w, kl_g.w, kl_b.w)),
	                   max(vec3(ij_r.y, ij_g.y, ij_b.y), vec3(kl_r.x, kl_g.x, kl_b.x)));
	vec3 accumulated = vec3(0.0);
	float weight_sum = 0.0;
	fsr_tap(accumulated, weight_sum, vec2(0, -1) - phase, direction, length_scale, lobe, clip_point, vec3(bc_r.x, bc_g.x, bc_b.x));
	fsr_tap(accumulated, weight_sum, vec2(1, -1) - phase, direction, length_scale, lobe, clip_point, vec3(bc_r.y, bc_g.y, bc_b.y));
	fsr_tap(accumulated, weight_sum, vec2(-1, 1) - phase, direction, length_scale, lobe, clip_point, vec3(ij_r.x, ij_g.x, ij_b.x));
	fsr_tap(accumulated, weight_sum, vec2(0, 1) - phase, direction, length_scale, lobe, clip_point, vec3(ij_r.y, ij_g.y, ij_b.y));
	fsr_tap(accumulated, weight_sum, vec2(0, 0) - phase, direction, length_scale, lobe, clip_point, vec3(ij_r.z, ij_g.z, ij_b.z));
	fsr_tap(accumulated, weight_sum, vec2(-1, 0) - phase, direction, length_scale, lobe, clip_point, vec3(ij_r.w, ij_g.w, ij_b.w));
	fsr_tap(accumulated, weight_sum, vec2(1, 1) - phase, direction, length_scale, lobe, clip_point, vec3(kl_r.x, kl_g.x, kl_b.x));
	fsr_tap(accumulated, weight_sum, vec2(2, 1) - phase, direction, length_scale, lobe, clip_point, vec3(kl_r.y, kl_g.y, kl_b.y));
	fsr_tap(accumulated, weight_sum, vec2(2, 0) - phase, direction, length_scale, lobe, clip_point, vec3(kl_r.z, kl_g.z, kl_b.z));
	fsr_tap(accumulated, weight_sum, vec2(1, 0) - phase, direction, length_scale, lobe, clip_point, vec3(kl_r.w, kl_g.w, kl_b.w));
	fsr_tap(accumulated, weight_sum, vec2(1, 2) - phase, direction, length_scale, lobe, clip_point, vec3(on_r.z, on_g.z, on_b.z));
	fsr_tap(accumulated, weight_sum, vec2(0, 2) - phase, direction, length_scale, lobe, clip_point, vec3(on_r.w, on_g.w, on_b.w));
	vec3 color = clamp(accumulated / max(weight_sum, 1e-5), minimum, maximum);
	vec2 alpha_uv = (position + 0.5) * pc.viewport.xy;
	return vec4(color, textureLod(source_color, alpha_uv, 0.0).a);
}

float safe_reciprocal(float value)
{
	return abs(value) < 1e-6 ? (value < 0.0 ? -1e6 : 1e6) : 1.0 / value;
}

vec4 fsr_rcas(vec2 uv)
{
	vec2 texel = pc.viewport.xy;
	vec3 north = textureLod(source_color, uv - vec2(0.0, texel.y), 0.0).rgb;
	vec3 west = textureLod(source_color, uv - vec2(texel.x, 0.0), 0.0).rgb;
	vec4 center_sample = textureLod(source_color, uv, 0.0);
	vec3 east = textureLod(source_color, uv + vec2(texel.x, 0.0), 0.0).rgb;
	vec3 south = textureLod(source_color, uv + vec2(0.0, texel.y), 0.0).rgb;
	vec3 minimum_ring = min(min(north, west), min(east, south));
	vec3 maximum_ring = max(max(north, west), max(east, south));
	vec3 hit_min = min(minimum_ring, center_sample.rgb) * vec3(safe_reciprocal(4.0 * maximum_ring.r),
	                                                           safe_reciprocal(4.0 * maximum_ring.g),
	                                                           safe_reciprocal(4.0 * maximum_ring.b));
	vec3 hit_max = (vec3(1.0) - max(maximum_ring, center_sample.rgb)) *
	               vec3(safe_reciprocal(4.0 * minimum_ring.r - 4.0),
	                    safe_reciprocal(4.0 * minimum_ring.g - 4.0),
	                    safe_reciprocal(4.0 * minimum_ring.b - 4.0));
	vec3 lobe_rgb = max(-hit_min, hit_max);
	float lobe = max(-(0.25 - 1.0 / 16.0), min(max(lobe_rgb.r, max(lobe_rgb.g, lobe_rgb.b)), 0.0));
	lobe *= exp2(-pc.sharpness);
	vec3 color = (lobe * (north + west + east + south) + center_sample.rgb) * safe_reciprocal(4.0 * lobe + 1.0);
	return vec4(srgb_to_linear(color), center_sample.a);
}

vec4 nis_copy()
{
	// NVScaler writes display-referred RGB into an aligned RGBA16F image.
	// Sample only the requested target rectangle and return linear RGB for the
	// sRGB OpenXR swapchain attachment.
	vec4 color = textureLod(source_color, gl_FragCoord.xy * pc.viewport.xy, 0.0);
	return vec4(srgb_to_linear(color.rgb), color.a);
}

void main()
{
	if (algorithm == 1)
		out_color = sgsr1(in_uv);
	else if (algorithm == 2)
		out_color = fsr1(in_uv);
	else if (algorithm == 4)
		out_color = fsr_rcas(in_uv);
	else if (algorithm == 5)
		out_color = nis_copy();
	else
		out_color = textureLod(source_color, in_uv, 0.0);
	if (pc.premultiply_alpha != 0)
	{
		if (out_color.a < 0.02)
			out_color = vec4(0.0);
		else
			out_color.rgb *= out_color.a;
	}
}

#endif
