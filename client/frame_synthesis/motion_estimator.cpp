/*
 * WiVRn VR streaming
 * Copyright (C) 2026  WiVRn contributors
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include "motion_estimator.h"

#include "qcom_motion_estimator.h"

namespace frame_synthesis
{

std::unique_ptr<motion_estimator> create_motion_estimator()
{
	return qcom_motion_estimator::probe();
}

std::string_view to_string(motion_estimator_backend backend) noexcept
{
	switch (backend)
	{
	case motion_estimator_backend::none:
		return "none";
	case motion_estimator_backend::qcom_gles:
		return "qcom_gles";
	}
	return "unknown";
}

std::string_view to_string(motion_estimation_status status) noexcept
{
	switch (status)
	{
	case motion_estimation_status::generated:
		return "generated";
	case motion_estimation_status::unavailable:
		return "unavailable";
	case motion_estimation_status::invalid_request:
		return "invalid_request";
	case motion_estimation_status::driver_error:
		return "driver_error";
	}
	return "unknown";
}

} // namespace frame_synthesis
