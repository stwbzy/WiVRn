/*
 * WiVRn VR streaming
 * Copyright (C) 2026 WiVRn-fsr contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include "fsr_config.h"
#include "vk/allocation.h"
#include "wivrn_packets.h"
#include "xrt/xrt_defines.h"

#include <array>
#include <vulkan/vulkan_raii.hpp>

namespace wivrn
{
struct vk_bundle;

vk::Extent3D fsr_output_extent(const from_headset::headset_info_packet &);

class fsr_upscaler
{
public:
	struct output
	{
		std::array<vk::ImageView, 2> views;
		std::array<xrt_rect, 2> rects;
	};

	fsr_upscaler(vk_bundle &, vk::Extent3D input_extent, vk::Extent3D output_extent);

	output upscale(vk::raii::Device &,
	               vk::raii::CommandBuffer &,
	               const std::array<vk::ImageView, 2> & source,
	               const std::array<xrt_rect, 2> & source_rect,
	               bool source_is_linear);

	vk::Extent3D input_size() const
	{
		return input_extent;
	}
	vk::Extent3D output_size() const
	{
		return output_extent;
	}

private:
	vk::Extent3D input_extent;
	vk::Extent3D output_extent;
	image_allocation easu_image;
	image_allocation output_image;
	std::array<vk::raii::ImageView, 2> easu_views;
	std::array<vk::raii::ImageView, 2> output_views;
	vk::raii::Sampler sampler;
	vk::raii::DescriptorSetLayout descriptor_set_layout;
	vk::raii::PipelineLayout easu_layout;
	vk::raii::PipelineLayout rcas_layout;
	vk::raii::Pipeline easu_pipeline;
	vk::raii::Pipeline rcas_pipeline;
	vk::raii::DescriptorPool descriptor_pool;
	// One EASU and one RCAS descriptor set per eye.
	std::array<vk::DescriptorSet, 4> descriptor_sets{};
	bool easu_valid = false;
	bool output_valid = false;
};
} // namespace wivrn
