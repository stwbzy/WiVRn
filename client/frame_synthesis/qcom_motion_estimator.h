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

#include "motion_estimator.h"

#include <cstdint>
#include <memory>
#include <string>

#if defined(__ANDROID__)
#include <android/hardware_buffer.h>
#else
struct AHardwareBuffer;
#endif

namespace frame_synthesis
{

/**
 * Small, dependency-free GLES loader surface.  The default Android probe
 * fills this through eglGetProcAddress obtained from libEGL at runtime.  It is
 * public so a caller with an already-owned EGL loader, and host tests, can
 * inject the same operations without linking against GLES.
 */
struct qcom_gl_api
{
	using get_string_fn = const unsigned char * (*)(uint32_t name);
	using get_integer_fn = void (*)(uint32_t name, int32_t * value);
	using get_error_fn = uint32_t (*)();
	using get_proc_address_fn = void * (*)(const char * name);

	get_string_fn get_string = nullptr;
	get_integer_fn get_integer = nullptr;
	get_error_fn get_error = nullptr;
	get_proc_address_fn get_proc_address = nullptr;
};

/**
 * Owns the reduced RGBA16F flow buffer and the native fence exported for the
 * GLES work which fills it.  The AHardwareBuffer pointer is borrowed from the
 * object; callers must keep the qcom_flow_output alive while using it.
 */
class qcom_flow_output final
{
public:
	qcom_flow_output() = default;
	~qcom_flow_output();

	qcom_flow_output(const qcom_flow_output &) = delete;
	qcom_flow_output & operator=(const qcom_flow_output &) = delete;
	qcom_flow_output(qcom_flow_output && other) noexcept;
	qcom_flow_output & operator=(qcom_flow_output && other) noexcept;

	AHardwareBuffer * hardware_buffer() const noexcept
	{
		return buffer_;
	}

	uint32_t width() const noexcept
	{
		return width_;
	}

	uint32_t height() const noexcept
	{
		return height_;
	}

	uint32_t block_width() const noexcept
	{
		return block_width_;
	}

	uint32_t block_height() const noexcept
	{
		return block_height_;
	}

	int fence_fd() const noexcept
	{
		return fence_fd_;
	}

	bool available() const noexcept
	{
		return buffer_ != nullptr && fence_fd_ >= 0;
	}

private:
	friend class qcom_motion_estimator;

	qcom_flow_output(AHardwareBuffer * buffer,
	                 uint32_t width,
	                 uint32_t height,
	                 uint32_t block_width,
	                 uint32_t block_height,
	                 int fence_fd) noexcept;

	void reset() noexcept;

	AHardwareBuffer * buffer_ = nullptr;
	uint32_t width_ = 0;
	uint32_t height_ = 0;
	uint32_t block_width_ = 0;
	uint32_t block_height_ = 0;
	int fence_fd_ = -1;
};

struct qcom_ahardware_buffer_result
{
	motion_estimation_status status = motion_estimation_status::unavailable;
	std::unique_ptr<qcom_flow_output> output;
	std::string message;

	bool generated() const noexcept
	{
		return status == motion_estimation_status::generated && output && output->available();
	}
};

class qcom_motion_estimator final : public motion_estimator
{
public:
	static constexpr const char * extension_name = "GL_QCOM_motion_estimation";

	/** Probe the current Android GLES context using a dynamic EGL loader. */
	static std::unique_ptr<qcom_motion_estimator> probe();

	/** Probe using caller-supplied entry points; useful for embedding/tests. */
	static std::unique_ptr<qcom_motion_estimator> probe(qcom_gl_api api);

	explicit qcom_motion_estimator(qcom_gl_api api = {});
	~qcom_motion_estimator() override;

	const motion_estimator_capabilities & capabilities() const noexcept override
	{
		return capabilities_;
	}

	motion_estimation_result estimate(const motion_estimation_request & request) override;

	/**
	 * Import two decoder-owned AHardwareBuffers, reduce their color/luma into
	 * R8 textures, run GL_QCOM_motion_estimation, and return an RGBA16F flow
	 * AHardwareBuffer plus an EGL native-fence fd.  The Android implementation
	 * owns its offscreen ES 3 context; host builds return unavailable.
	 */
	qcom_ahardware_buffer_result estimate_ahardware_buffers(
	        AHardwareBuffer * reference,
	        AHardwareBuffer * target,
	        uint32_t width,
	        uint32_t height);

	bool hardware_buffer_path_available() const noexcept
	{
		return capabilities_.hardware_buffer_path;
	}

private:
	using estimate_motion_fn = void (*)(uint32_t reference, uint32_t target, uint32_t output);
	using estimate_regions_fn = void (*)(uint32_t reference, uint32_t target, uint32_t output, uint32_t mask);

	qcom_gl_api api_;
	estimate_motion_fn estimate_motion_ = nullptr;
	estimate_regions_fn estimate_regions_ = nullptr;
	motion_estimator_capabilities capabilities_;

#if defined(__ANDROID__)
	class egl_context;
	std::unique_ptr<egl_context> egl_context_;
#endif

	void resolve_core_entry_points();
	void query_block_dimensions();
};

} // namespace frame_synthesis
