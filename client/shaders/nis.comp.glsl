// The MIT License (MIT)
//
// Copyright (c) 2022 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
//
// WiVRn integration of NVIDIA Image Scaling SDK 1.0.3 NVScaler. The scaler
// body is kept in NIS_Scaler.h; this wrapper adapts sRGB OpenXR images and
// stores the official filter banks in a read-only buffer.

#version 450
#extension GL_GOOGLE_include_directive : enable

#define NIS_GLSL 1
#define NIS_SCALER 1
#define NIS_USE_HALF_PRECISION 0
#define NIS_BLOCK_WIDTH 32
#define NIS_BLOCK_HEIGHT 24
#define NIS_THREAD_GROUP_SIZE 256
#define NIS_CLAMP_OUTPUT 1

layout(set = 0, binding = 0) uniform const_buffer
{
	float kDetectRatio;
	float kDetectThres;
	float kMinContrastRatio;
	float kRatioNorm;

	float kContrastBoost;
	float kEps;
	float kSharpStartY;
	float kSharpScaleY;

	float kSharpStrengthMin;
	float kSharpStrengthScale;
	float kSharpLimitMin;
	float kSharpLimitScale;

	float kScaleX;
	float kScaleY;
	float kDstNormX;
	float kDstNormY;
	float kSrcNormX;
	float kSrcNormY;

	uint kInputViewportOriginX;
	uint kInputViewportOriginY;
	uint kInputViewportWidth;
	uint kInputViewportHeight;

	uint kOutputViewportOriginX;
	uint kOutputViewportOriginY;
	uint kOutputViewportWidth;
	uint kOutputViewportHeight;

	float reserved0;
	float reserved1;
};

layout(set = 0, binding = 1) uniform sampler samplerLinearClamp;
layout(set = 0, binding = 2) uniform texture2D in_texture;
layout(rgba16f, set = 0, binding = 3) uniform writeonly image2D out_texture;
layout(std430, set = 0, binding = 4) readonly buffer coefficient_buffer
{
	float coefficient[];
} nis_coefficients;

vec3 linearToSrgb(vec3 color)
{
	vec3 low = color * 12.92;
	vec3 high = 1.055 * pow(max(color, vec3(0.0)), vec3(1.0 / 2.4)) - 0.055;
	return mix(high, low, lessThanEqual(color, vec3(0.0031308)));
}

vec4 nisSampleDisplay(texture2D image, sampler image_sampler, vec2 uv)
{
	vec4 color = textureLod(sampler2D(image, image_sampler), uv, 0.0);
	return vec4(linearToSrgb(color.rgb), color.a);
}

#include "NIS_Scaler.h"

layout(local_size_x = NIS_THREAD_GROUP_SIZE) in;

void main()
{
	NVScaler(gl_WorkGroupID.xy, gl_LocalInvocationID.x);
}
