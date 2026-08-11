/*
 * WiVRn VR streaming
 * Copyright (C) 2026 WiVRn-fsr contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include "vk/allocation.h"
#include "wivrn_packets.h"

#include <array>
#include <optional>
#include <vector>
#include <vulkan/vulkan_raii.hpp>

class stream_upscaler
{
	static constexpr uint32_t view_count = 2;
	vk::raii::Device & device;
	wivrn::from_headset::spatial_upscaling_mode requested_mode;
	wivrn::from_headset::spatial_upscaling_mode active_mode;
	vk::Extent2D input_extent;
	vk::Extent2D output_extent;

	image_allocation input_image;
	std::array<vk::raii::ImageView, view_count> input_views;
	std::optional<image_allocation> fsr_image;
	std::vector<vk::raii::ImageView> fsr_views;
	std::optional<image_allocation> nis_image;
	vk::Extent2D nis_extent;
	std::vector<vk::raii::ImageView> nis_views;
	std::optional<buffer_allocation> nis_config_buffer;
	std::optional<buffer_allocation> nis_coefficient_buffer;
	vk::raii::Sampler sampler = nullptr;
	vk::raii::RenderPass renderpass = nullptr;
	vk::raii::DescriptorSetLayout descriptor_set_layout = nullptr;
	vk::raii::DescriptorPool descriptor_pool = nullptr;
	std::array<vk::DescriptorSet, view_count * 2> descriptor_sets{};
	vk::raii::PipelineLayout pipeline_layout = nullptr;
	vk::raii::Pipeline bilinear_pipeline = nullptr;
	vk::raii::Pipeline algorithm_pipeline = nullptr;
	vk::raii::Pipeline fsr_rcas_pipeline = nullptr;
	vk::raii::DescriptorSetLayout nis_descriptor_set_layout = nullptr;
	vk::raii::DescriptorPool nis_descriptor_pool = nullptr;
	std::array<vk::DescriptorSet, view_count> nis_descriptor_sets{};
	vk::raii::PipelineLayout nis_pipeline_layout = nullptr;
	vk::raii::Pipeline nis_pipeline = nullptr;
	bool nis_initialized = false;
	std::vector<vk::raii::ImageView> output_views;
	std::vector<vk::raii::Framebuffer> framebuffers;
	std::vector<vk::raii::Framebuffer> fsr_framebuffers;

	vk::raii::Pipeline make_pipeline(uint32_t algorithm);
	void initialize_nis();

public:
	stream_upscaler(vk::raii::Device & device,
	                const std::vector<vk::Image> & output_images,
	                vk::Extent2D input_extent,
	                vk::Extent2D output_extent,
	                vk::Format format,
	                wivrn::from_headset::spatial_upscaling_mode mode);

	stream_upscaler(const stream_upscaler &) = delete;

	std::vector<vk::Image> input_images() const;
	bool using_fallback() const
	{
		return active_mode != requested_mode;
	}
	wivrn::from_headset::spatial_upscaling_mode mode() const
	{
		return active_mode;
	}
	vk::Extent2D input_size() const
	{
		return input_extent;
	}

	void upscale(vk::raii::CommandBuffer & command_buffer, int destination, bool premultiply_alpha);
};
