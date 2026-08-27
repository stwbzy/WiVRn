/*
 * WiVRn VR streaming
 * Copyright (C) 2026  WiVRn contributors
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include "stream_synthesizer.h"

#include "vk/allocation.h"
#include "vk/pipeline.h"
#include "vk/shader.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <spdlog/spdlog.h>
#include <vk_mem_alloc.h>

namespace scenes
{
namespace
{

constexpr uint32_t history_layers = stream_synthesizer::view_count;
constexpr uint32_t flow_local_size = 8;
constexpr vk::Format flow_format = vk::Format::eR16G16B16A16Sfloat;
constexpr uint64_t invalid_frame = std::numeric_limits<uint64_t>::max();

bool has_format_features(vk::FormatFeatureFlags available, vk::FormatFeatureFlags required)
{
	return (available & required) == required;
}

vk::ImageSubresourceRange color_range(uint32_t layers = history_layers)
{
	return {
	        .aspectMask = vk::ImageAspectFlagBits::eColor,
	        .baseMipLevel = 0,
	        .levelCount = 1,
	        .baseArrayLayer = 0,
	        .layerCount = layers,
	};
}

vk::PipelineStageFlags stage_for_layout(vk::ImageLayout layout)
{
	switch (layout)
	{
	case vk::ImageLayout::eColorAttachmentOptimal:
		return vk::PipelineStageFlagBits::eColorAttachmentOutput;
	case vk::ImageLayout::eTransferDstOptimal:
	case vk::ImageLayout::eTransferSrcOptimal:
		return vk::PipelineStageFlagBits::eTransfer;
	case vk::ImageLayout::eShaderReadOnlyOptimal:
		return vk::PipelineStageFlagBits::eComputeShader | vk::PipelineStageFlagBits::eFragmentShader;
	case vk::ImageLayout::eGeneral:
		return vk::PipelineStageFlagBits::eComputeShader;
	case vk::ImageLayout::eUndefined:
		return vk::PipelineStageFlagBits::eTopOfPipe;
	default:
		return vk::PipelineStageFlagBits::eAllCommands;
	}
}

vk::AccessFlags access_for_layout(vk::ImageLayout layout)
{
	switch (layout)
	{
	case vk::ImageLayout::eColorAttachmentOptimal:
		return vk::AccessFlagBits::eColorAttachmentWrite;
	case vk::ImageLayout::eTransferDstOptimal:
		return vk::AccessFlagBits::eTransferWrite;
	case vk::ImageLayout::eTransferSrcOptimal:
		return vk::AccessFlagBits::eTransferRead;
	case vk::ImageLayout::eShaderReadOnlyOptimal:
		return vk::AccessFlagBits::eShaderRead;
	case vk::ImageLayout::eGeneral:
		return vk::AccessFlagBits::eShaderRead | vk::AccessFlagBits::eShaderWrite;
	case vk::ImageLayout::eUndefined:
		return {};
	default:
		return vk::AccessFlagBits::eMemoryRead | vk::AccessFlagBits::eMemoryWrite;
	}
}

void transition(
        vk::raii::CommandBuffer & command_buffer,
        vk::Image image,
        vk::ImageLayout old_layout,
        vk::ImageLayout new_layout,
        vk::ImageSubresourceRange range)
{
	if (not image || old_layout == new_layout)
		return;

	const vk::ImageMemoryBarrier barrier{
	        .srcAccessMask = access_for_layout(old_layout),
	        .dstAccessMask = access_for_layout(new_layout),
	        .oldLayout = old_layout,
	        .newLayout = new_layout,
	        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
	        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
	        .image = image,
	        .subresourceRange = range,
	};
	command_buffer.pipelineBarrier(
	        stage_for_layout(old_layout),
	        stage_for_layout(new_layout),
	        {},
	        {},
	        {},
	        barrier);
}

uint32_t ceil_div(uint32_t value, uint32_t divisor)
{
	return (value + divisor - 1) / divisor;
}

vk::ImageViewCreateInfo array_view_info(vk::Image image, vk::Format format)
{
	return {
	        .image = image,
	        .viewType = vk::ImageViewType::e2DArray,
	        .format = format,
	        .components = {
	                .r = vk::ComponentSwizzle::eIdentity,
	                .g = vk::ComponentSwizzle::eIdentity,
	                .b = vk::ComponentSwizzle::eIdentity,
	                .a = vk::ComponentSwizzle::eIdentity,
	        },
	        .subresourceRange = color_range(),
	};
}

vk::ImageCreateInfo array_image_info(vk::Format format, vk::Extent2D extent, vk::ImageUsageFlags usage)
{
	return {
	        .imageType = vk::ImageType::e2D,
	        .format = format,
	        .extent = {
	                .width = extent.width,
	                .height = extent.height,
	                .depth = 1,
	        },
	        .mipLevels = 1,
	        .arrayLayers = history_layers,
	        .samples = vk::SampleCountFlagBits::e1,
	        .tiling = vk::ImageTiling::eOptimal,
	        .usage = usage,
	        .initialLayout = vk::ImageLayout::eUndefined,
	};
}

struct flow_view_constants
{
	std::array<float, 4> q_rel{};             // x, y, z, w; current view -> previous view
	std::array<float, 4> current_tan_rect{}; // left, right, up, down
	std::array<float, 4> previous_tan_rect{};
	bool valid = false;
};

struct alignas(16) flow_push_constants
{
	std::array<float, 4> input_size{}; // width, height, inverse width, inverse height
	std::array<float, 4> parameters{}; // search radius, level scale, valid-eye mask, reserved
	std::array<std::array<float, 4>, history_layers> q_rel{};
	std::array<std::array<float, 4>, history_layers> current_tan_rect{};
	std::array<std::array<float, 4>, history_layers> previous_tan_rect{};
};
static_assert(sizeof(flow_push_constants) == 128);

struct alignas(16) warp_push_constants
{
	std::array<float, 4> input_size{};
	std::array<float, 4> parameters{};
};
static_assert(sizeof(warp_push_constants) == 32);

bool finite_quaternion(const XrQuaternionf & orientation)
{
	return std::isfinite(orientation.x) && std::isfinite(orientation.y) &&
	       std::isfinite(orientation.z) && std::isfinite(orientation.w);
}

bool finite_fov(const XrFovf & fov)
{
	return std::isfinite(fov.angleLeft) && std::isfinite(fov.angleRight) &&
	       std::isfinite(fov.angleUp) && std::isfinite(fov.angleDown);
}

bool make_tangent_rect(const XrFovf & fov, std::array<float, 4> & tangent_rect)
{
	if (not finite_fov(fov))
		return false;

	const float left = std::tan(fov.angleLeft);
	const float right = std::tan(fov.angleRight);
	const float up = std::tan(fov.angleUp);
	const float down = std::tan(fov.angleDown);
	if (not(std::isfinite(left) && std::isfinite(right) && std::isfinite(up) && std::isfinite(down)))
		return false;
	if (right - left <= std::numeric_limits<float>::epsilon() || up - down <= std::numeric_limits<float>::epsilon())
		return false;

	tangent_rect = {left, right, up, down};
	return true;
}

flow_view_constants make_flow_view_constants(const XrPosef & previous_pose, const XrPosef & current_pose, const XrFovf & previous_fov, const XrFovf & current_fov)
{
	flow_view_constants result;
	if (not finite_quaternion(previous_pose.orientation) || not finite_quaternion(current_pose.orientation))
		return result;

	const glm::quat previous_q{
	        previous_pose.orientation.w,
	        previous_pose.orientation.x,
	        previous_pose.orientation.y,
	        previous_pose.orientation.z,
	};
	const glm::quat current_q{
	        current_pose.orientation.w,
	        current_pose.orientation.x,
	        current_pose.orientation.y,
	        current_pose.orientation.z,
	};
	const float previous_length_squared = glm::dot(previous_q, previous_q);
	const float current_length_squared = glm::dot(current_q, current_q);
	if (not(std::isfinite(previous_length_squared) && std::isfinite(current_length_squared)) ||
	    previous_length_squared <= 1e-8f || current_length_squared <= 1e-8f)
		return result;

	if (not make_tangent_rect(previous_fov, result.previous_tan_rect) ||
	    not make_tangent_rect(current_fov, result.current_tan_rect))
		return result;

	// q_rel maps a ray in the current eye frame into the previous eye frame.
	// This removes head rotation from the optical-flow search; OpenXR remains
	// responsible for the final late pose/time warp.
	const glm::quat previous_normalized = glm::normalize(previous_q);
	const glm::quat current_normalized = glm::normalize(current_q);
	const glm::quat relative = glm::normalize(glm::inverse(previous_normalized) * current_normalized);
	if (not(std::isfinite(relative.x) && std::isfinite(relative.y) && std::isfinite(relative.z) && std::isfinite(relative.w)))
		return result;

	result.q_rel = {relative.x, relative.y, relative.z, relative.w};
	result.valid = true;
	return result;
}

} // namespace

struct stream_synthesizer::impl
{
	struct history_slot
	{
		image_allocation image;
		vk::raii::ImageView view = nullptr;
		vk::ImageLayout layout = vk::ImageLayout::eUndefined;
		bool valid = false;
		frame_metadata metadata{};
	};

	struct flow_slot
	{
		image_allocation image;
		vk::raii::ImageView view = nullptr;
		vk::ImageLayout layout = vk::ImageLayout::eUndefined;
		vk::Extent2D extent{};
	};

	vk::raii::Device & device;
	vk::raii::PhysicalDevice & physical_device;
	vk::Extent2D extent;
	vk::Format format;
	float confidence_threshold;

	state state_ = state::unavailable;
	std::string reason = "not initialized";

	std::array<history_slot, 2> history;
	std::array<flow_slot, 2> flow;
	int latest_history = -1;
	uint64_t synthesized_frame_id = invalid_frame;
	uint64_t synthesized_generation = invalid_frame;

	vk::Extent2D coarse_extent;
	vk::Extent2D refine_extent;
	vk::raii::Sampler sampler = nullptr;

	vk::raii::DescriptorPool descriptor_pool = nullptr;
	vk::raii::DescriptorSetLayout coarse_descriptor_layout = nullptr;
	vk::raii::DescriptorSetLayout refine_descriptor_layout = nullptr;
	vk::raii::DescriptorSetLayout warp_descriptor_layout = nullptr;
	vk::DescriptorSet coarse_descriptor_set = nullptr;
	vk::DescriptorSet refine_descriptor_set = nullptr;
	vk::DescriptorSet warp_descriptor_set = nullptr;

	vk::raii::PipelineLayout coarse_pipeline_layout = nullptr;
	vk::raii::PipelineLayout refine_pipeline_layout = nullptr;
	vk::raii::PipelineLayout warp_pipeline_layout = nullptr;
	vk::raii::Pipeline coarse_pipeline = nullptr;
	vk::raii::Pipeline refine_pipeline = nullptr;
	vk::raii::Pipeline warp_pipeline = nullptr;

	vk::raii::RenderPass render_pass = nullptr;
	std::vector<vk::Image> output_images;
	std::vector<vk::raii::ImageView> output_views;
	std::vector<vk::raii::Framebuffer> framebuffers;

	impl(vk::raii::Device & device, vk::raii::PhysicalDevice & physical_device, configuration config) :
	        device(device),
	        physical_device(physical_device),
	        extent(config.extent),
	        format(config.format),
	        confidence_threshold(std::clamp(config.confidence_threshold, 0.0f, 1.0f))
	{
		output_images.assign(config.output_images.begin(), config.output_images.end());

		try
		{
			initialize();
		}
		catch (const std::exception & e)
		{
			state_ = state::unavailable;
			reason = std::string("Vulkan client synthesizer unavailable: ") + e.what();
			spdlog::warn("{}", reason);
		}
		catch (...)
		{
			state_ = state::unavailable;
			reason = "Vulkan client synthesizer unavailable: unknown initialization failure";
			spdlog::warn("{}", reason);
		}
	}

	void disable(std::string message)
	{
		state_ = state::unavailable;
		reason = std::move(message);
	}

	void initialize()
	{
		if (extent.width == 0 || extent.height == 0)
			return disable("invalid zero stream extent");
		if (format == vk::Format::eUndefined)
			return disable("stream color format is undefined");
		if (output_images.empty())
			return disable("no output swapchain images were supplied");

		const auto color_properties = physical_device.getFormatProperties(format);
		const auto required_color_features = vk::FormatFeatureFlagBits::eSampledImage |
		                                     vk::FormatFeatureFlagBits::eTransferSrc |
		                                     vk::FormatFeatureFlagBits::eTransferDst |
		                                     vk::FormatFeatureFlagBits::eColorAttachment;
		if (not has_format_features(color_properties.optimalTilingFeatures, required_color_features))
			return disable("stream format lacks sampled, transfer-source, transfer-destination or color-attachment support");

		const auto flow_properties = physical_device.getFormatProperties(flow_format);
		const auto required_flow_features = vk::FormatFeatureFlagBits::eSampledImage |
		                                    vk::FormatFeatureFlagBits::eStorageImage;
		if (not has_format_features(flow_properties.optimalTilingFeatures, required_flow_features))
			return disable("R16G16B16A16Sfloat lacks sampled/storage-image support");

		coarse_extent = {
		        .width = std::max(1u, ceil_div(extent.width, 16)),
		        .height = std::max(1u, ceil_div(extent.height, 16)),
		};
		refine_extent = {
		        .width = std::max(1u, ceil_div(extent.width, 4)),
		        .height = std::max(1u, ceil_div(extent.height, 4)),
		};

		const auto sampler_features = color_properties.optimalTilingFeatures;
		const auto filter = (sampler_features & vk::FormatFeatureFlagBits::eSampledImageFilterLinear) != vk::FormatFeatureFlags{}
		                            ? vk::Filter::eLinear
		                            : vk::Filter::eNearest;
		sampler = vk::raii::Sampler(
		        device,
		        vk::SamplerCreateInfo{
		                .magFilter = filter,
		                .minFilter = filter,
		                .mipmapMode = vk::SamplerMipmapMode::eNearest,
		                .addressModeU = vk::SamplerAddressMode::eClampToEdge,
		                .addressModeV = vk::SamplerAddressMode::eClampToEdge,
		                .addressModeW = vk::SamplerAddressMode::eClampToEdge,
		                .maxAnisotropy = 1.0f,
		        });

		create_history_images();
		create_flow_images();
		create_render_targets();
		create_descriptor_state();
		create_pipelines();

		state_ = state::ready;
		reason.clear();
	}

	void create_history_images()
	{
		for (auto & slot: history)
		{
			slot.image = image_allocation(
			        device,
			        array_image_info(
			                format,
			                extent,
			                vk::ImageUsageFlagBits::eTransferDst | vk::ImageUsageFlagBits::eSampled),
			        VmaAllocationCreateInfo{.usage = VMA_MEMORY_USAGE_AUTO},
			        "ssw real-frame history");
			slot.view = vk::raii::ImageView(device, array_view_info(slot.image, format));
		}
	}

	void create_flow_images()
	{
		for (size_t level = 0; level < flow.size(); ++level)
		{
			flow[level].extent = level == 0 ? coarse_extent : refine_extent;
			flow[level].image = image_allocation(
			        device,
			        array_image_info(
			                flow_format,
			                flow[level].extent,
			                vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled),
			        VmaAllocationCreateInfo{.usage = VMA_MEMORY_USAGE_AUTO},
			        level == 0 ? "ssw coarse flow" : "ssw refined flow");
			flow[level].view = vk::raii::ImageView(device, array_view_info(flow[level].image, flow_format));
		}
	}

	void create_render_targets()
	{
		const vk::AttachmentDescription attachment{
		        .format = format,
		        .samples = vk::SampleCountFlagBits::e1,
		        .loadOp = vk::AttachmentLoadOp::eDontCare,
		        .storeOp = vk::AttachmentStoreOp::eStore,
		        .stencilLoadOp = vk::AttachmentLoadOp::eDontCare,
		        .stencilStoreOp = vk::AttachmentStoreOp::eDontCare,
		        .initialLayout = vk::ImageLayout::eColorAttachmentOptimal,
		        .finalLayout = vk::ImageLayout::eColorAttachmentOptimal,
		};
		const vk::AttachmentReference color_reference{
		        .attachment = 0,
		        .layout = vk::ImageLayout::eColorAttachmentOptimal,
		};
		const vk::SubpassDescription subpass{
		        .pipelineBindPoint = vk::PipelineBindPoint::eGraphics,
		        .colorAttachmentCount = 1,
		        .pColorAttachments = &color_reference,
		};
		render_pass = vk::raii::RenderPass(
		        device,
		        vk::RenderPassCreateInfo{
		                .attachmentCount = 1,
		                .pAttachments = &attachment,
		                .subpassCount = 1,
		                .pSubpasses = &subpass,
		        });

		output_views.reserve(output_images.size() * history_layers);
		framebuffers.reserve(output_images.size() * history_layers);
		for (vk::Image image: output_images)
		{
			if (not image)
				throw std::runtime_error("null output swapchain image");

			for (uint32_t view = 0; view < history_layers; ++view)
			{
				vk::ImageViewCreateInfo view_info{
				        .image = image,
				        .viewType = vk::ImageViewType::e2D,
				        .format = format,
				        .components = {
				                .r = vk::ComponentSwizzle::eIdentity,
				                .g = vk::ComponentSwizzle::eIdentity,
				                .b = vk::ComponentSwizzle::eIdentity,
				                .a = vk::ComponentSwizzle::eIdentity,
				        },
				        .subresourceRange = {
				                .aspectMask = vk::ImageAspectFlagBits::eColor,
				                .baseMipLevel = 0,
				                .levelCount = 1,
				                .baseArrayLayer = view,
				                .layerCount = 1,
				        },
				};
				output_views.emplace_back(device, view_info);

				vk::FramebufferCreateInfo framebuffer_info{
				        .renderPass = *render_pass,
				        .attachmentCount = 1,
				        .pAttachments = &*output_views.back(),
				        .width = extent.width,
				        .height = extent.height,
				        .layers = 1,
				};
				framebuffers.emplace_back(device, framebuffer_info);
			}
		}
	}

	vk::raii::DescriptorSetLayout make_descriptor_layout(std::span<const vk::DescriptorSetLayoutBinding> bindings)
	{
		return vk::raii::DescriptorSetLayout(
		        device,
		        vk::DescriptorSetLayoutCreateInfo{
		                .bindingCount = static_cast<uint32_t>(bindings.size()),
		                .pBindings = bindings.data(),
		        });
	}

	vk::raii::PipelineLayout make_pipeline_layout(vk::DescriptorSetLayout descriptor_layout, uint32_t push_constant_size)
	{
		const vk::PushConstantRange push_constants{
		        .stageFlags = vk::ShaderStageFlagBits::eCompute,
		        .offset = 0,
		        .size = push_constant_size,
		};
		return vk::raii::PipelineLayout(
		        device,
		        vk::PipelineLayoutCreateInfo{
		                .setLayoutCount = 1,
		                .pSetLayouts = &descriptor_layout,
		                .pushConstantRangeCount = 1,
		                .pPushConstantRanges = &push_constants,
		        });
	}

	void create_descriptor_state()
	{
		const std::array pool_sizes{
		        vk::DescriptorPoolSize{
		                .type = vk::DescriptorType::eCombinedImageSampler,
		                .descriptorCount = 7,
		        },
		        vk::DescriptorPoolSize{
		                .type = vk::DescriptorType::eStorageImage,
		                .descriptorCount = 2,
		        },
		};
		descriptor_pool = vk::raii::DescriptorPool(
		        device,
		        vk::DescriptorPoolCreateInfo{
		                .maxSets = 3,
		                .poolSizeCount = static_cast<uint32_t>(pool_sizes.size()),
		                .pPoolSizes = pool_sizes.data(),
		        });

		const std::array coarse_bindings{
		        vk::DescriptorSetLayoutBinding{
		                .binding = 0,
		                .descriptorType = vk::DescriptorType::eCombinedImageSampler,
		                .descriptorCount = 1,
		                .stageFlags = vk::ShaderStageFlagBits::eCompute,
		        },
		        vk::DescriptorSetLayoutBinding{
		                .binding = 1,
		                .descriptorType = vk::DescriptorType::eCombinedImageSampler,
		                .descriptorCount = 1,
		                .stageFlags = vk::ShaderStageFlagBits::eCompute,
		        },
		        vk::DescriptorSetLayoutBinding{
		                .binding = 2,
		                .descriptorType = vk::DescriptorType::eStorageImage,
		                .descriptorCount = 1,
		                .stageFlags = vk::ShaderStageFlagBits::eCompute,
		        },
		};
		const std::array refine_bindings{
		        vk::DescriptorSetLayoutBinding{
		                .binding = 0,
		                .descriptorType = vk::DescriptorType::eCombinedImageSampler,
		                .descriptorCount = 1,
		                .stageFlags = vk::ShaderStageFlagBits::eCompute,
		        },
		        vk::DescriptorSetLayoutBinding{
		                .binding = 1,
		                .descriptorType = vk::DescriptorType::eCombinedImageSampler,
		                .descriptorCount = 1,
		                .stageFlags = vk::ShaderStageFlagBits::eCompute,
		        },
		        vk::DescriptorSetLayoutBinding{
		                .binding = 2,
		                .descriptorType = vk::DescriptorType::eCombinedImageSampler,
		                .descriptorCount = 1,
		                .stageFlags = vk::ShaderStageFlagBits::eCompute,
		        },
		        vk::DescriptorSetLayoutBinding{
		                .binding = 3,
		                .descriptorType = vk::DescriptorType::eStorageImage,
		                .descriptorCount = 1,
		                .stageFlags = vk::ShaderStageFlagBits::eCompute,
		        },
		};
		const std::array warp_bindings{
		        vk::DescriptorSetLayoutBinding{
		                .binding = 0,
		                .descriptorType = vk::DescriptorType::eCombinedImageSampler,
		                .descriptorCount = 1,
		                .stageFlags = vk::ShaderStageFlagBits::eFragment,
		        },
		        vk::DescriptorSetLayoutBinding{
		                .binding = 1,
		                .descriptorType = vk::DescriptorType::eCombinedImageSampler,
		                .descriptorCount = 1,
		                .stageFlags = vk::ShaderStageFlagBits::eFragment,
		        },
		};

		coarse_descriptor_layout = make_descriptor_layout(coarse_bindings);
		refine_descriptor_layout = make_descriptor_layout(refine_bindings);
		warp_descriptor_layout = make_descriptor_layout(warp_bindings);

		coarse_descriptor_set = device.allocateDescriptorSets(vk::DescriptorSetAllocateInfo{
		                                                         .descriptorPool = *descriptor_pool,
		                                                         .descriptorSetCount = 1,
		                                                         .pSetLayouts = &*coarse_descriptor_layout,
	                                                 })[0]
		                              .release();
		refine_descriptor_set = device.allocateDescriptorSets(vk::DescriptorSetAllocateInfo{
	                                                         .descriptorPool = *descriptor_pool,
	                                                         .descriptorSetCount = 1,
	                                                         .pSetLayouts = &*refine_descriptor_layout,
	                                                 })[0]
	                              .release();
		warp_descriptor_set = device.allocateDescriptorSets(vk::DescriptorSetAllocateInfo{
	                                                       .descriptorPool = *descriptor_pool,
	                                                       .descriptorSetCount = 1,
	                                                       .pSetLayouts = &*warp_descriptor_layout,
	                                               })[0]
	                            .release();

		coarse_pipeline_layout = make_pipeline_layout(*coarse_descriptor_layout, sizeof(flow_push_constants));
		refine_pipeline_layout = make_pipeline_layout(*refine_descriptor_layout, sizeof(flow_push_constants));

		const vk::PushConstantRange warp_push_constants_range{
		        .stageFlags = vk::ShaderStageFlagBits::eFragment,
		        .offset = 0,
		        .size = sizeof(warp_push_constants),
		};
		warp_pipeline_layout = vk::raii::PipelineLayout(
		        device,
		        vk::PipelineLayoutCreateInfo{
		                .setLayoutCount = 1,
		                .pSetLayouts = &*warp_descriptor_layout,
		                .pushConstantRangeCount = 1,
		                .pPushConstantRanges = &warp_push_constants_range,
	        });
	}

	vk::raii::Pipeline make_compute_pipeline(const char * shader_name, vk::PipelineLayout layout)
	{
		auto shader = load_shader(device, shader_name);
		return vk::raii::Pipeline(
		        device,
		        nullptr,
		        vk::ComputePipelineCreateInfo{
		                .stage = {
		                        .stage = vk::ShaderStageFlagBits::eCompute,
		                        .module = *shader,
		                        .pName = "main",
		                },
		                .layout = layout,
	        });
	}

	void create_pipelines()
	{
		coarse_pipeline = make_compute_pipeline("ssw_flow_coarse", *coarse_pipeline_layout);
		refine_pipeline = make_compute_pipeline("ssw_flow_refine", *refine_pipeline_layout);

		auto vertex_shader = load_shader(device, "ssw_warp.vert");
		auto fragment_shader = load_shader(device, "ssw_warp.frag");
		vk::pipeline_builder pipeline_info{
		        .flags = {},
		        .Stages = {
		                {
		                        .stage = vk::ShaderStageFlagBits::eVertex,
		                        .module = *vertex_shader,
		                        .pName = "main",
		                },
		                {
		                        .stage = vk::ShaderStageFlagBits::eFragment,
		                        .module = *fragment_shader,
		                        .pName = "main",
		                },
		        },
		        .InputAssemblyState = {{
		                .topology = vk::PrimitiveTopology::eTriangleList,
	        }},
	        .Viewports = {{}},
	        .Scissors = {{}},
	        .RasterizationState = {{
	                .polygonMode = vk::PolygonMode::eFill,
	                .cullMode = vk::CullModeFlagBits::eNone,
	                .frontFace = vk::FrontFace::eCounterClockwise,
	                .lineWidth = 1.0f,
	        }},
	        .MultisampleState = {{
	                .rasterizationSamples = vk::SampleCountFlagBits::e1,
	        }},
	        .ColorBlendState = {.flags = {}},
	        .ColorBlendAttachments = {{
	                .blendEnable = VK_FALSE,
	                .colorWriteMask = vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
	                                   vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA,
	        }},
	        .DynamicStates = {vk::DynamicState::eViewport, vk::DynamicState::eScissor},
	        .layout = *warp_pipeline_layout,
	        .renderPass = *render_pass,
		};
		const vk::GraphicsPipelineCreateInfo graphics_info = pipeline_info;
		warp_pipeline = vk::raii::Pipeline(device, nullptr, graphics_info);
	}

	vk::DescriptorImageInfo sampled_info(vk::ImageView view) const
	{
		return {
		        .sampler = *sampler,
		        .imageView = view,
		        .imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
		};
	}

	vk::DescriptorImageInfo storage_info(vk::ImageView view) const
	{
		return {
		        .sampler = nullptr,
		        .imageView = view,
		        .imageLayout = vk::ImageLayout::eGeneral,
		};
	}

	flow_push_constants make_flow_push_constants(int previous_index, int current_index, float search_radius, float level_scale) const
	{
		flow_push_constants constants{
		        .input_size = {
		                float(extent.width),
		                float(extent.height),
		                1.0f / float(extent.width),
		                1.0f / float(extent.height),
		        },
		        .parameters = {search_radius, level_scale, 0.0f, 0.0f},
		};

		uint32_t valid_mask = 0;
		for (uint32_t eye = 0; eye < history_layers; ++eye)
		{
			const auto view = make_flow_view_constants(
			        history[previous_index].metadata.pose[eye],
			        history[current_index].metadata.pose[eye],
			        history[previous_index].metadata.fov[eye],
			        history[current_index].metadata.fov[eye]);
			constants.q_rel[eye] = view.q_rel;
			constants.current_tan_rect[eye] = view.current_tan_rect;
			constants.previous_tan_rect[eye] = view.previous_tan_rect;
			if (view.valid)
				valid_mask |= 1u << eye;
		}
		constants.parameters[2] = float(valid_mask);
		return constants;
	}

	void dispatch_flow(vk::raii::CommandBuffer & command_buffer, int previous_index, int current_index)
	{
		const auto range = color_range();
		transition(
		        command_buffer,
		        flow[0].image,
		        flow[0].layout,
		        vk::ImageLayout::eGeneral,
		        range);
		flow[0].layout = vk::ImageLayout::eGeneral;
		transition(
		        command_buffer,
		        flow[1].image,
		        flow[1].layout,
		        vk::ImageLayout::eGeneral,
		        range);
		flow[1].layout = vk::ImageLayout::eGeneral;

		std::array coarse_infos{
		        sampled_info(*history[previous_index].view),
		        sampled_info(*history[current_index].view),
		        storage_info(*flow[0].view),
		};
		std::array coarse_writes{
		        vk::WriteDescriptorSet{
		                .dstSet = coarse_descriptor_set,
		                .dstBinding = 0,
		                .descriptorCount = 1,
		                .descriptorType = vk::DescriptorType::eCombinedImageSampler,
		                .pImageInfo = &coarse_infos[0],
		        },
		        vk::WriteDescriptorSet{
		                .dstSet = coarse_descriptor_set,
		                .dstBinding = 1,
		                .descriptorCount = 1,
		                .descriptorType = vk::DescriptorType::eCombinedImageSampler,
		                .pImageInfo = &coarse_infos[1],
		        },
		        vk::WriteDescriptorSet{
		                .dstSet = coarse_descriptor_set,
		                .dstBinding = 2,
		                .descriptorCount = 1,
		                .descriptorType = vk::DescriptorType::eStorageImage,
		                .pImageInfo = &coarse_infos[2],
		        },
		};
		device.updateDescriptorSets(coarse_writes, {});

		command_buffer.bindPipeline(vk::PipelineBindPoint::eCompute, *coarse_pipeline);
		command_buffer.bindDescriptorSets(
		        vk::PipelineBindPoint::eCompute,
		        *coarse_pipeline_layout,
		        0,
		        coarse_descriptor_set,
		        {});
		const flow_push_constants coarse_pc = make_flow_push_constants(previous_index, current_index, 4.0f, 16.0f);
		command_buffer.pushConstants<flow_push_constants>(
		        *coarse_pipeline_layout,
		        vk::ShaderStageFlagBits::eCompute,
		        0,
		        coarse_pc);
		command_buffer.dispatch(
		        ceil_div(coarse_extent.width, flow_local_size),
		        ceil_div(coarse_extent.height, flow_local_size),
		        history_layers);

		transition(
		        command_buffer,
		        flow[0].image,
		        vk::ImageLayout::eGeneral,
		        vk::ImageLayout::eShaderReadOnlyOptimal,
		        range);
		flow[0].layout = vk::ImageLayout::eShaderReadOnlyOptimal;

		std::array refine_infos{
		        sampled_info(*history[previous_index].view),
		        sampled_info(*history[current_index].view),
		        sampled_info(*flow[0].view),
		        storage_info(*flow[1].view),
		};
		std::array refine_writes{
		        vk::WriteDescriptorSet{
		                .dstSet = refine_descriptor_set,
		                .dstBinding = 0,
		                .descriptorCount = 1,
		                .descriptorType = vk::DescriptorType::eCombinedImageSampler,
		                .pImageInfo = &refine_infos[0],
		        },
		        vk::WriteDescriptorSet{
		                .dstSet = refine_descriptor_set,
		                .dstBinding = 1,
		                .descriptorCount = 1,
		                .descriptorType = vk::DescriptorType::eCombinedImageSampler,
		                .pImageInfo = &refine_infos[1],
		        },
		        vk::WriteDescriptorSet{
		                .dstSet = refine_descriptor_set,
		                .dstBinding = 2,
		                .descriptorCount = 1,
		                .descriptorType = vk::DescriptorType::eCombinedImageSampler,
		                .pImageInfo = &refine_infos[2],
		        },
		        vk::WriteDescriptorSet{
		                .dstSet = refine_descriptor_set,
		                .dstBinding = 3,
		                .descriptorCount = 1,
		                .descriptorType = vk::DescriptorType::eStorageImage,
		                .pImageInfo = &refine_infos[3],
		        },
		};
		device.updateDescriptorSets(refine_writes, {});

		command_buffer.bindPipeline(vk::PipelineBindPoint::eCompute, *refine_pipeline);
		command_buffer.bindDescriptorSets(
		        vk::PipelineBindPoint::eCompute,
		        *refine_pipeline_layout,
		        0,
		        refine_descriptor_set,
		        {});
		const flow_push_constants refine_pc = make_flow_push_constants(previous_index, current_index, 2.0f, 4.0f);
		command_buffer.pushConstants<flow_push_constants>(
		        *refine_pipeline_layout,
		        vk::ShaderStageFlagBits::eCompute,
		        0,
		        refine_pc);
		command_buffer.dispatch(
		        ceil_div(refine_extent.width, flow_local_size),
		        ceil_div(refine_extent.height, flow_local_size),
		        history_layers);

		transition(
		        command_buffer,
		        flow[1].image,
		        vk::ImageLayout::eGeneral,
		        vk::ImageLayout::eShaderReadOnlyOptimal,
		        range);
		flow[1].layout = vk::ImageLayout::eShaderReadOnlyOptimal;
	}

	capture_result capture_real(
	        vk::raii::CommandBuffer & command_buffer,
	        image_ref source,
	        const frame_metadata & metadata)
	{
		if (state_ != state::ready)
			return {.status = capture_status::unavailable};
		if (not source.image)
			return {.status = capture_status::invalid};

		bool reset_before_capture = metadata.discontinuity;
		if (latest_history >= 0)
		{
			const auto & latest = history[latest_history].metadata;
			if (metadata.connection_generation != latest.connection_generation)
				reset_before_capture = true;
			else if (not reset_before_capture && metadata.frame_id <= latest.frame_id)
				return {.status = capture_status::duplicate};
		}
		if (reset_before_capture)
			reset_history();

		const int destination_index = latest_history < 0 ? 0 : 1 - latest_history;
		auto & destination = history[destination_index];
		const auto range = color_range();

		try
		{
			transition(
			        command_buffer,
			        source.image,
			        source.layout,
			        vk::ImageLayout::eTransferSrcOptimal,
			        range);
			transition(
			        command_buffer,
			        destination.image,
			        destination.layout,
			        vk::ImageLayout::eTransferDstOptimal,
			        range);
			destination.layout = vk::ImageLayout::eTransferDstOptimal;

			command_buffer.copyImage(
			        source.image,
			        vk::ImageLayout::eTransferSrcOptimal,
			        destination.image,
			        vk::ImageLayout::eTransferDstOptimal,
			        vk::ImageCopy{
			                .srcSubresource = {
			                        .aspectMask = vk::ImageAspectFlagBits::eColor,
			                        .baseArrayLayer = 0,
			                        .layerCount = history_layers,
			                },
			                .dstSubresource = {
			                        .aspectMask = vk::ImageAspectFlagBits::eColor,
			                        .baseArrayLayer = 0,
			                        .layerCount = history_layers,
			                },
			                .extent = {
			                        .width = extent.width,
			                        .height = extent.height,
			                        .depth = 1,
			                },
			        });

			transition(
			        command_buffer,
			        source.image,
			        vk::ImageLayout::eTransferSrcOptimal,
			        source.layout,
			        range);
			transition(
			        command_buffer,
			        destination.image,
			        vk::ImageLayout::eTransferDstOptimal,
			        vk::ImageLayout::eShaderReadOnlyOptimal,
			        range);
			destination.layout = vk::ImageLayout::eShaderReadOnlyOptimal;
			destination.metadata = metadata;
			destination.valid = true;
			latest_history = destination_index;
			synthesized_frame_id = invalid_frame;
			synthesized_generation = invalid_frame;
		}
		catch (const std::exception & e)
		{
			disable(std::string("capture_real command recording failed: ") + e.what());
			return {.status = capture_status::unavailable};
		}

		return {.status = reset_before_capture ? capture_status::reset_and_captured : capture_status::captured};
	}

	synthesis_result synthesize(vk::raii::CommandBuffer & command_buffer, image_ref destination)
	{
		if (state_ != state::ready)
			return {.status = synthesis_status::unavailable};
		if (not destination.image)
			return {.status = synthesis_status::invalid};
		if (latest_history < 0 || not history[latest_history].valid)
			return {.status = synthesis_status::no_history};

		const int previous_index = 1 - latest_history;
		if (not history[previous_index].valid)
			return {.status = synthesis_status::no_history};

		const auto & current = history[latest_history].metadata;
		if (synthesized_frame_id == current.frame_id && synthesized_generation == current.connection_generation)
			return {.status = synthesis_status::already_generated};

		auto output_iter = std::find(output_images.begin(), output_images.end(), destination.image);
		if (output_iter == output_images.end())
			return {.status = synthesis_status::invalid};

		try
		{
			dispatch_flow(command_buffer, previous_index, latest_history);

			transition(
			        command_buffer,
			        destination.image,
			        destination.layout,
			        vk::ImageLayout::eColorAttachmentOptimal,
			        color_range());

			const size_t output_index = static_cast<size_t>(std::distance(output_images.begin(), output_iter));
			const std::array warp_infos{
			        sampled_info(*history[latest_history].view),
			        sampled_info(*flow[1].view),
			};
			const std::array warp_writes{
			        vk::WriteDescriptorSet{
			                .dstSet = warp_descriptor_set,
			                .dstBinding = 0,
			                .descriptorCount = 1,
			                .descriptorType = vk::DescriptorType::eCombinedImageSampler,
			                .pImageInfo = &warp_infos[0],
			        },
			        vk::WriteDescriptorSet{
			                .dstSet = warp_descriptor_set,
			                .dstBinding = 1,
			                .descriptorCount = 1,
			                .descriptorType = vk::DescriptorType::eCombinedImageSampler,
			                .pImageInfo = &warp_infos[1],
			        },
			};
			device.updateDescriptorSets(warp_writes, {});

			command_buffer.bindPipeline(vk::PipelineBindPoint::eGraphics, *warp_pipeline);
			command_buffer.bindDescriptorSets(
			        vk::PipelineBindPoint::eGraphics,
			        *warp_pipeline_layout,
			        0,
			        warp_descriptor_set,
			        {});

			for (uint32_t view = 0; view < history_layers; ++view)
			{
				const size_t framebuffer_index = output_index * history_layers + view;
				const vk::RenderPassBeginInfo begin_info{
				        .renderPass = *render_pass,
				        .framebuffer = *framebuffers[framebuffer_index],
				        .renderArea = {
				                .offset = {0, 0},
				                .extent = extent,
				        },
				};
				command_buffer.beginRenderPass(begin_info, vk::SubpassContents::eInline);
				command_buffer.setViewport(
				        0,
				        vk::Viewport{
				                .x = 0.0f,
				                .y = 0.0f,
				                .width = static_cast<float>(extent.width),
				                .height = static_cast<float>(extent.height),
				                .minDepth = 0.0f,
				                .maxDepth = 1.0f,
				        });
				command_buffer.setScissor(0, vk::Rect2D{.offset = {0, 0}, .extent = extent});
				const warp_push_constants pc{
				        .input_size = {float(extent.width), float(extent.height), 1.0f / float(extent.width), 1.0f / float(extent.height)},
				        .parameters = {0.5f, confidence_threshold, float(view), 0.0f},
				};
				command_buffer.pushConstants<warp_push_constants>(
				        *warp_pipeline_layout,
				        vk::ShaderStageFlagBits::eFragment,
				        0,
				        pc);
				command_buffer.draw(3, 1, 0, 0);
				command_buffer.endRenderPass();
			}

			synthesized_frame_id = current.frame_id;
			synthesized_generation = current.connection_generation;
		}
		catch (const std::exception & e)
		{
			disable(std::string("synthesize command recording failed: ") + e.what());
			return {.status = synthesis_status::unavailable};
		}

		return {.status = synthesis_status::generated};
	}

	void reset_history() noexcept
	{
		for (auto & slot: history)
			slot.valid = false;
		latest_history = -1;
		synthesized_frame_id = invalid_frame;
		synthesized_generation = invalid_frame;
	}

	void reset() noexcept
	{
		reset_history();
	}

	std::optional<frame_metadata> latest_real_metadata() const
	{
		if (latest_history < 0 || not history[latest_history].valid)
			return std::nullopt;
		return history[latest_history].metadata;
	}
};

stream_synthesizer::stream_synthesizer(
        vk::raii::Device & device,
        vk::raii::PhysicalDevice & physical_device,
        configuration config) :
        impl_(std::make_unique<impl>(device, physical_device, std::move(config)))
{}

stream_synthesizer::~stream_synthesizer() = default;

stream_synthesizer::stream_synthesizer(stream_synthesizer &&) noexcept = default;

stream_synthesizer & stream_synthesizer::operator=(stream_synthesizer &&) noexcept = default;

stream_synthesizer::state stream_synthesizer::status() const noexcept
{
	return impl_ ? impl_->state_ : state::unavailable;
}

std::string_view stream_synthesizer::unavailable_reason() const noexcept
{
	static constexpr std::string_view no_impl = "synthesizer implementation is not present";
	return impl_ ? std::string_view(impl_->reason) : no_impl;
}

stream_synthesizer::capture_result stream_synthesizer::capture_real(
        vk::raii::CommandBuffer & command_buffer,
        image_ref source,
        const frame_metadata & metadata)
{
	return impl_ ? impl_->capture_real(command_buffer, source, metadata) : capture_result{};
}

stream_synthesizer::synthesis_result stream_synthesizer::synthesize(
        vk::raii::CommandBuffer & command_buffer,
        image_ref destination)
{
	return impl_ ? impl_->synthesize(command_buffer, destination) : synthesis_result{};
}

void stream_synthesizer::reset() noexcept
{
	if (impl_)
		impl_->reset();
}

std::optional<stream_synthesizer::frame_metadata> stream_synthesizer::latest_real_metadata() const
{
	return impl_ ? impl_->latest_real_metadata() : std::nullopt;
}

} // namespace scenes
