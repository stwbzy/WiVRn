/*
 * WiVRn VR streaming
 * Copyright (C) 2026  WiVRn contributors
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace frame_synthesis
{

/**
 * A backend is reported only after its required runtime entry points have
 * been found.  In particular, the presence of an Adreno GPU does not imply
 * that the QCOM GLES extension is usable by the current context.
 */
enum class motion_estimator_backend : uint8_t
{
	none,
	qcom_gles,
};

enum class motion_estimation_status : uint8_t
{
	generated,
	unavailable,
	invalid_request,
	driver_error,
};

struct motion_estimator_capabilities
{
	motion_estimator_backend backend = motion_estimator_backend::none;
	bool extension_advertised = false;
	bool estimates_motion = false;
	bool estimates_regions = false;
	bool can_check_errors = false;
	// True only when the Android EGL/AHardwareBuffer path was fully probed.
	// The generic texture-id backend may still be usable when this is false.
	bool hardware_buffer_path = false;

	// GL_QCOM_motion_estimation reports one vector per search block.  A value
	// of zero means that the context did not answer the optional query; it
	// does not mean that the extension is absent.
	uint32_t search_block_width = 0;
	uint32_t search_block_height = 0;

	std::string reason;

	bool available() const noexcept
	{
		return backend != motion_estimator_backend::none && estimates_motion;
	}
};

/**
 * Textures are owned by the caller and must be valid in the current GLES
 * context.  The QCOM extension requires R8 reference/target textures and an
 * RGBA16F output texture (and an R8UI mask for region estimation).  This
 * module deliberately does not create or import textures: doing so would
 * couple it to a particular EGL/AHardwareBuffer/Vulkan interop path.
 */
struct motion_estimation_request
{
	uint32_t reference_texture = 0;
	uint32_t target_texture = 0;
	uint32_t output_texture = 0;
	uint32_t mask_texture = 0;
	bool estimate_regions = false;
};

struct motion_estimation_result
{
	motion_estimation_status status = motion_estimation_status::unavailable;
	uint32_t api_error = 0;
	std::string message;

	bool generated() const noexcept
	{
		return status == motion_estimation_status::generated;
	}
};

class motion_estimator
{
public:
	virtual ~motion_estimator() = default;

	virtual const motion_estimator_capabilities & capabilities() const noexcept = 0;
	virtual motion_estimation_result estimate(const motion_estimation_request & request) = 0;
};

/**
 * Creates the best runtime backend.  On non-Android builds, or when there is
 * no current GLES context/extension, the returned object is a functional
 * unavailable backend and every estimate() call reports that fallback state.
 */
std::unique_ptr<motion_estimator> create_motion_estimator();

std::string_view to_string(motion_estimator_backend backend) noexcept;
std::string_view to_string(motion_estimation_status status) noexcept;

} // namespace frame_synthesis
