/*
 * WiVRn VR streaming
 * Copyright (C) 2026  WiVRn contributors
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include "frame_synthesis/motion_estimator.h"
#include "frame_synthesis/qcom_motion_estimator.h"

#include <cstdint>
#include <cstring>
#include <iostream>
#include <utility>

namespace
{

constexpr uint32_t gl_extensions = 0x1F03;
constexpr uint32_t gl_motion_estimation_search_block_x_qcom = 0x8C90;
constexpr uint32_t gl_motion_estimation_search_block_y_qcom = 0x8C91;

bool advertise_qcom = false;
bool advertise_procs = true;
uint32_t motion_calls = 0;
uint32_t region_calls = 0;

const unsigned char * fake_get_string(uint32_t name)
{
	if (name != gl_extensions)
		return nullptr;
	return advertise_qcom ? reinterpret_cast<const unsigned char *>("GL_EXT_other GL_QCOM_motion_estimation GL_EXT_tail")
	                      : reinterpret_cast<const unsigned char *>("GL_EXT_other GL_EXT_tail");
}

void fake_get_integer(uint32_t name, int32_t * value)
{
	if (name == gl_motion_estimation_search_block_x_qcom || name == gl_motion_estimation_search_block_y_qcom)
		*value = 8;
	else
		*value = 0;
}

uint32_t fake_get_error()
{
	return 0;
}

void fake_estimate_motion(uint32_t, uint32_t, uint32_t)
{
	++motion_calls;
}

void fake_estimate_regions(uint32_t, uint32_t, uint32_t, uint32_t)
{
	++region_calls;
}

template<typename Function>
void * function_address(Function function)
{
	return reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(function));
}

void * fake_get_proc(const char * name)
{
	if (std::strcmp(name, "glTexEstimateMotionQCOM") == 0)
		return advertise_procs ? function_address(fake_estimate_motion) : nullptr;
	if (std::strcmp(name, "glTexEstimateMotionRegionsQCOM") == 0)
		return advertise_procs ? function_address(fake_estimate_regions) : nullptr;
	return nullptr;
}

bool check(bool condition, const char * message)
{
	if (condition)
		return true;
	std::cerr << "qcom_motion_estimator_test: " << message << '\n';
	return false;
}

frame_synthesis::qcom_gl_api fake_api()
{
	return {
	        .get_string = fake_get_string,
	        .get_integer = fake_get_integer,
	        .get_error = fake_get_error,
	        .get_proc_address = fake_get_proc,
	};
}

} // namespace

int main()
{
	bool passed = true;

	// The ordinary factory is a deliberate unavailable stub on host builds.
	auto native = frame_synthesis::create_motion_estimator();
	passed &= check(native && !native->capabilities().available(), "host factory must report capability=false");
	passed &= check(!native->estimate({.reference_texture = 1, .target_texture = 2, .output_texture = 3}).generated(),
	                "host fallback must not claim generated vectors");
	passed &= check(!native->capabilities().hardware_buffer_path,
	                "host factory must report AHardwareBuffer path unavailable");
	frame_synthesis::qcom_motion_estimator host_qcom;
	const auto host_buffer_result = host_qcom.estimate_ahardware_buffers(nullptr, nullptr, 8, 8);
	passed &= check(host_buffer_result.status == frame_synthesis::motion_estimation_status::unavailable &&
	                        !host_buffer_result.output && !host_buffer_result.generated(),
	                "host AHardwareBuffer path must return an explicit unavailable result");
	frame_synthesis::qcom_flow_output empty_output;
	frame_synthesis::qcom_flow_output moved_output = std::move(empty_output);
	passed &= check(!empty_output.available() && !moved_output.available() && moved_output.fence_fd() < 0,
	                "flow output move/RAII state must stay empty without Android resources");

	advertise_qcom = false;
	auto missing_extension = frame_synthesis::qcom_motion_estimator::probe(fake_api());
	passed &= check(!missing_extension->capabilities().available(), "missing extension must disable backend");

	advertise_qcom = true;
	advertise_procs = false;
	auto missing_proc = frame_synthesis::qcom_motion_estimator::probe(fake_api());
	passed &= check(!missing_proc->capabilities().available(), "missing motion proc must disable backend");
	advertise_procs = true;
	auto missing_error_api = fake_api();
	missing_error_api.get_error = nullptr;
	auto missing_error = frame_synthesis::qcom_motion_estimator::probe(missing_error_api);
	passed &= check(!missing_error->capabilities().available(), "missing GL error checking must disable backend");

	motion_calls = 0;
	region_calls = 0;
	auto available = frame_synthesis::qcom_motion_estimator::probe(fake_api());
	passed &= check(available->capabilities().available(), "advertised extension and procs must enable backend");
	passed &= check(available->capabilities().search_block_width == 8 && available->capabilities().search_block_height == 8,
	                "block dimensions must be queried");

	auto result = available->estimate({.reference_texture = 1, .target_texture = 2, .output_texture = 3});
	passed &= check(result.generated() && motion_calls == 1, "primary estimation must call QCOM proc");

	result = available->estimate({.reference_texture = 1, .target_texture = 2, .output_texture = 3, .estimate_regions = true});
	passed &= check(result.status == frame_synthesis::motion_estimation_status::invalid_request,
	                "region estimation without a mask must be rejected");

	result = available->estimate({.reference_texture = 1,
	                              .target_texture = 2,
	                              .output_texture = 3,
	                              .mask_texture = 4,
	                              .estimate_regions = true});
	passed &= check(result.generated() && region_calls == 1, "region estimation must call QCOM proc");

	return passed ? 0 : 1;
}
