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
#version 450

// RelaxedPrecision is a hint on Vulkan.  Adreno can execute the SGSR scalar
// math at mediump while implementations without efficient FP16 retain FP32.
precision mediump float;
precision highp int;

layout(push_constant) uniform pc
{
	ivec4 rgb_rect;
	ivec4 a_rect;
	vec4 scale;
	vec4 bias;
};

#ifdef VERT_SHADER

layout (location = 0) in vec2 vPosition;
layout (location = 1) in uvec2 vUV;

layout(location = 0) out vec4 outUV;

void main()
{
	gl_Position = vec4(vPosition, 0.0, 1.0);
	vec2 uv = vUV;
	outUV.xy = (uv + rgb_rect.xy) / rgb_rect.zw;
	outUV.zw = (uv + a_rect.xy) / a_rect.zw;
}
#endif

#ifdef FRAG_SHADER

layout(constant_id = 0) const int alpha = 1;
layout(constant_id = 1) const bool do_srgb = false;
layout(constant_id = 2) const bool use_sgsr = false;

layout(set = 0, binding = 0) uniform sampler2D rgb[alpha + 1];

layout(location = 0) in vec4 inUV;

layout(location = 0) out vec4 outColor;

float sRGB_to_linear(float x)
{
	if (x <= 0.04045)
		return x / 12.92;
	return pow((x + 0.055) / 1.055, 2.4);
}

vec4 sRGB_to_linear_rgba(vec4 x)
{
	return vec4(
	        sRGB_to_linear(x.r),
	        sRGB_to_linear(x.g),
	        sRGB_to_linear(x.b),
	        x.a);
}

// Snapdragon Game Super Resolution 1 mobile, no edge-direction variant.
// Adapted from Qualcomm's BSD-3-Clause sgsr1_shader_mobile.frag.  The decoded
// image is filtered in its UNORM/gamma representation and converted at most
// once after reconstruction; no pow() is executed for individual taps.
float sgsr_fast_lanczos2(float x)
{
	float a = x - 4.0;
	float b = x * a - a;
	a *= a;
	return b * a;
}

vec2 sgsr_weight(highp vec2 delta, float contrast, float deviation)
{
	float x = dot(delta, delta) * 0.55 + clamp(abs(contrast) * deviation, 0.0, 1.0);
	float weight = sgsr_fast_lanczos2(x);
	return vec2(weight, weight * contrast);
}

vec3 sgsr_mobile_no_direction(sampler2D image, highp vec2 uv, highp vec2 inv_size)
{
	vec3 color = textureLod(image, uv, 0.0).rgb;
	const vec2 offsets[12] = vec2[12](
	        vec2(-1.0, -2.0), vec2(0.0, -2.0),
	        vec2(-1.0, 1.0), vec2(0.0, 1.0),
	        vec2(1.0, -1.0), vec2(0.0, -1.0),
	        vec2(0.0, 0.0), vec2(1.0, 0.0),
		        vec2(-1.0, -1.0), vec2(-2.0, -1.0),
		        vec2(-2.0, 0.0), vec2(-1.0, 0.0));
	highp vec2 image_coord = uv / inv_size + vec2(-0.5, 0.5);
	highp vec2 base_uv = (floor(image_coord) + vec2(0.5)) * inv_size;
	float taps[12];
	for (int i = 0; i < 12; ++i)
		taps[i] = textureLod(image, base_uv + offsets[i] * inv_size, 0.0).g;

	// The VR threshold recommended by Qualcomm is 4/255.  Below it the native
	// bilinear sample is both cheaper and more stable on flat regions.
	float edge_vote = abs(taps[6] - taps[5]) + abs(color.g - taps[5]) + abs(color.g - taps[6]);
	if (edge_vote <= 4.0 / 255.0)
		return color;

	float mean = (taps[5] + taps[6] + taps[8] + taps[11]) * 0.25;
	float sum = 0.0;
	for (int i = 0; i < 12; ++i)
	{
		taps[i] -= mean;
		sum += abs(taps[i]);
	}
	float deviation = 2.181818 / max(sum, 1.0 / 65536.0);
	highp vec2 phase = fract(image_coord);
	vec2 weighted = vec2(0.0);
	for (int i = 0; i < 12; ++i)
		weighted += sgsr_weight(phase - offsets[i], taps[i], deviation);

	float filtered = weighted.y / max(abs(weighted.x), 1.0 / 65536.0);
	float minimum = min(min(taps[5], taps[6]), min(taps[8], taps[11]));
	float maximum = max(max(taps[5], taps[6]), max(taps[8], taps[11]));
	filtered = clamp(2.0 * filtered, minimum, maximum);
	float delta = clamp(filtered - (color.g - mean), -23.0 / 255.0, 23.0 / 255.0);
	return clamp(color + vec3(delta), vec3(0.0), vec3(1.0));
}

void main()
{
	vec3 reconstructed = use_sgsr
	                             ? sgsr_mobile_no_direction(rgb[0], inUV.xy, 1.0 / vec2(rgb_rect.zw))
	                             : texture(rgb[0], inUV.xy).rgb;
	if (alpha == 1)
	{
		// Avoid sampling between the eyes
		vec2 a = inUV.zw;
		float d = a.x - 0.5;
		if (abs(d) *a_rect.z < 1)
			a.x += (d > 0 ? 1 : -1)  / float(a_rect.z);
		outColor = vec4(reconstructed, texture(rgb[1], a).r);
	}
	else
		outColor = vec4(reconstructed, texture(rgb[0], inUV.xy).a);

	if (do_srgb)
	{
		outColor = sRGB_to_linear_rgba(outColor);
	}

	if (alpha == 0)
	{
		outColor = outColor * scale + bias;
	}
	else if (outColor.a < 0.02)
	{
		outColor = vec4(0);
	}
	else
	{
		outColor.rgb /= outColor.a;
		outColor = outColor * scale + bias;
		outColor.rgb *= outColor.a;
	}
}

#endif
