/*
 * WiVRn VR streaming
 * Copyright (C) 2026 WiVRn-fsr contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "fsr_upscaler.h"

#include "utils/wivrn_vk_bundle.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <format>
#include <ranges>
#include <stdexcept>

namespace
{
vk::raii::ImageView make_view(wivrn::vk_bundle & vk, image_allocation & image, uint32_t layer)
{
	return vk::raii::ImageView{
	        vk.device,
	        vk::ImageViewCreateInfo{
	                .image = image,
	                .viewType = vk::ImageViewType::e2D,
	                .format = vk::Format::eR16G16B16A16Sfloat,
	                .subresourceRange = {
	                        .aspectMask = vk::ImageAspectFlagBits::eColor,
	                        .levelCount = 1,
	                        .baseArrayLayer = layer,
	                        .layerCount = 1,
	                },
	        }};
}

vk::raii::Sampler make_sampler(wivrn::vk_bundle & vk)
{
	return vk::raii::Sampler{
	        vk.device,
	        vk::SamplerCreateInfo{
	                .magFilter = vk::Filter::eLinear,
	                .minFilter = vk::Filter::eLinear,
	                .mipmapMode = vk::SamplerMipmapMode::eNearest,
	                .addressModeU = vk::SamplerAddressMode::eClampToEdge,
	                .addressModeV = vk::SamplerAddressMode::eClampToEdge,
	                .addressModeW = vk::SamplerAddressMode::eClampToEdge,
	        }};
}

vk::raii::DescriptorSetLayout make_descriptor_set_layout(wivrn::vk_bundle & vk)
{
	const std::array bindings{
	        vk::DescriptorSetLayoutBinding{
	                .binding = 0,
	                .descriptorType = vk::DescriptorType::eCombinedImageSampler,
	                .descriptorCount = 1,
	                .stageFlags = vk::ShaderStageFlagBits::eCompute,
	        },
	        vk::DescriptorSetLayoutBinding{
	                .binding = 1,
	                .descriptorType = vk::DescriptorType::eStorageImage,
	                .descriptorCount = 1,
	                .stageFlags = vk::ShaderStageFlagBits::eCompute,
	        },
	};
	return vk::raii::DescriptorSetLayout{
	        vk.device,
	        vk::DescriptorSetLayoutCreateInfo{
	                .bindingCount = uint32_t(bindings.size()),
	                .pBindings = bindings.data(),
	        }};
}

struct alignas(16) easu_push_constants
{
	std::array<float, 4> con0;
	std::array<float, 4> con1;
	std::array<float, 4> con2;
	std::array<float, 4> con3;
	std::array<int32_t, 2> output_offset;
	std::array<uint32_t, 2> output_size;
	uint32_t source_is_linear;
	std::array<uint32_t, 3> padding{};
};
static_assert(sizeof(easu_push_constants) == 96);

struct alignas(16) rcas_push_constants
{
	std::array<int32_t, 2> output_offset;
	std::array<uint32_t, 2> output_size;
	float sharpness;
	std::array<float, 3> padding{};
};
static_assert(sizeof(rcas_push_constants) == 32);

vk::raii::PipelineLayout make_pipeline_layout(wivrn::vk_bundle & vk,
                                              vk::DescriptorSetLayout ds_layout,
                                              uint32_t push_constant_size)
{
	const vk::PushConstantRange range{
	        .stageFlags = vk::ShaderStageFlagBits::eCompute,
	        .size = push_constant_size,
	};
	return vk::raii::PipelineLayout{
	        vk.device,
	        vk::PipelineLayoutCreateInfo{
	                .setLayoutCount = 1,
	                .pSetLayouts = &ds_layout,
	                .pushConstantRangeCount = 1,
	                .pPushConstantRanges = &range,
	        }};
}

vk::raii::Pipeline make_pipeline(wivrn::vk_bundle & vk, vk::PipelineLayout layout, const char * shader_name)
{
	auto shader = vk.load_shader(shader_name);
	return vk::raii::Pipeline{
	        vk.device,
	        nullptr,
	        vk::ComputePipelineCreateInfo{
	                .stage = {
	                        .stage = vk::ShaderStageFlagBits::eCompute,
	                        .module = *shader,
	                        .pName = "main",
	                },
	                .layout = layout,
	        }};
}

vk::raii::DescriptorPool make_descriptor_pool(wivrn::vk_bundle & vk)
{
	const std::array sizes{
	        vk::DescriptorPoolSize{vk::DescriptorType::eCombinedImageSampler, 6},
	        vk::DescriptorPoolSize{vk::DescriptorType::eStorageImage, 6},
	};
	return vk::raii::DescriptorPool{
	        vk.device,
	        vk::DescriptorPoolCreateInfo{
	                .maxSets = 6,
	                .poolSizeCount = uint32_t(sizes.size()),
	                .pPoolSizes = sizes.data(),
	        }};
}

uint32_t divide_round_up(uint32_t value, uint32_t divisor)
{
	return (value + divisor - 1) / divisor;
}

xrt_rect scale_rect(const xrt_rect & rect, vk::Extent3D input, vk::Extent3D output)
{
	const auto scale_axis = [](int value, uint32_t source, uint32_t target) {
		return int(std::lround(double(value) * double(target) / double(source)));
	};

	xrt_rect result{
	        .offset = {
	                .w = scale_axis(rect.offset.w, input.width, output.width),
	                .h = scale_axis(rect.offset.h, input.height, output.height),
	        },
	        .extent = {
	                .w = scale_axis(rect.extent.w, input.width, output.width),
	                .h = scale_axis(rect.extent.h, input.height, output.height),
	        },
	};
	result.offset.w = std::clamp(result.offset.w, 0, int(output.width) - 1);
	result.offset.h = std::clamp(result.offset.h, 0, int(output.height) - 1);
	result.extent.w = std::clamp(result.extent.w, 1, int(output.width) - result.offset.w);
	result.extent.h = std::clamp(result.extent.h, 1, int(output.height) - result.offset.h);
	return result;
}

easu_push_constants make_easu_constants(const xrt_rect & input_rect,
                                        const xrt_rect & output_rect,
                                        vk::Extent3D input_extent,
                                        bool source_is_linear)
{
	const float viewport_x = input_rect.extent.w;
	const float viewport_y = input_rect.extent.h;
	const float output_x = output_rect.extent.w;
	const float output_y = output_rect.extent.h;
	const float input_x = input_extent.width;
	const float input_y = input_extent.height;

	return {
	        .con0 = {
	                viewport_x / output_x,
	                viewport_y / output_y,
	                0.5f * viewport_x / output_x - 0.5f + input_rect.offset.w,
	                0.5f * viewport_y / output_y - 0.5f + input_rect.offset.h,
	        },
	        .con1 = {1.0f / input_x, 1.0f / input_y, 1.0f / input_x, -1.0f / input_y},
	        .con2 = {-1.0f / input_x, 2.0f / input_y, 1.0f / input_x, 2.0f / input_y},
	        .con3 = {0.0f, 4.0f / input_y, 0.0f, 0.0f},
	        .output_offset = {output_rect.offset.w, output_rect.offset.h},
	        .output_size = {uint32_t(output_rect.extent.w), uint32_t(output_rect.extent.h)},
	        .source_is_linear = source_is_linear,
	};
}
} // namespace

namespace wivrn
{
vk::Extent3D fsr_output_extent(const from_headset::headset_info_packet & info)
{
	const auto output = fsr_output_dimensions(info);
	return {output[0], output[1], 1};
}

fsr_upscaler::fsr_upscaler(vk_bundle & vk, vk::Extent3D input_extent, vk::Extent3D output_extent) :
        input_extent(input_extent),
        output_extent(output_extent),
        easu_image{
                vk.device,
                vk::ImageCreateInfo{
                        .imageType = vk::ImageType::e2D,
                        .format = vk::Format::eR16G16B16A16Sfloat,
                        .extent = output_extent,
                        .mipLevels = 1,
                        .arrayLayers = 2,
                        .samples = vk::SampleCountFlagBits::e1,
                        .usage = vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled,
                },
                VmaAllocationCreateInfo{.usage = VMA_MEMORY_USAGE_AUTO},
                "FSR EASU output"},
        output_image{
                vk.device,
                vk::ImageCreateInfo{
                        .imageType = vk::ImageType::e2D,
                        .format = vk::Format::eR16G16B16A16Sfloat,
                        .extent = output_extent,
                        .mipLevels = 1,
                        .arrayLayers = 2,
                        .samples = vk::SampleCountFlagBits::e1,
                        .usage = vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled,
                },
                VmaAllocationCreateInfo{.usage = VMA_MEMORY_USAGE_AUTO},
                "FSR RCAS output"},
        easu_views{make_view(vk, easu_image, 0), make_view(vk, easu_image, 1)},
        output_views{make_view(vk, output_image, 0), make_view(vk, output_image, 1)},
        sampler(make_sampler(vk)),
        descriptor_set_layout(make_descriptor_set_layout(vk)),
        easu_layout(make_pipeline_layout(vk, *descriptor_set_layout, sizeof(easu_push_constants))),
        rcas_layout(make_pipeline_layout(vk, *descriptor_set_layout, sizeof(rcas_push_constants))),
        easu_pipeline(make_pipeline(vk, *easu_layout, "fsr_easu")),
        rcas_pipeline(make_pipeline(vk, *rcas_layout, "fsr_rcas")),
        descriptor_pool(make_descriptor_pool(vk))
{
	if (input_extent.width == 0 || input_extent.height == 0 || output_extent.width < input_extent.width ||
	    output_extent.height < input_extent.height ||
	    (output_extent.width == input_extent.width && output_extent.height == input_extent.height))
		throw std::invalid_argument("FSR output must be larger than its input on both axes");

	std::array<vk::DescriptorSetLayout, 6> layouts;
	layouts.fill(*descriptor_set_layout);
	auto sets = vk.device.allocateDescriptorSets(vk::DescriptorSetAllocateInfo{
	        .descriptorPool = descriptor_pool,
	        .descriptorSetCount = uint32_t(layouts.size()),
	        .pSetLayouts = layouts.data(),
	});
	for (auto [index, set]: std::ranges::enumerate_view(sets))
		descriptor_sets[index] = set;

	for (uint32_t view = 0; view < 2; ++view)
	{
		const vk::DescriptorImageInfo source_info{
		        .sampler = *sampler,
		        .imageView = *easu_views[view],
		        .imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
		};
		const vk::DescriptorImageInfo destination_info{
		        .imageView = *output_views[view],
		        .imageLayout = vk::ImageLayout::eGeneral,
		};
		const std::array writes{
		        vk::WriteDescriptorSet{
		                .dstSet = descriptor_sets[4 + view],
		                .dstBinding = 0,
		                .descriptorCount = 1,
		                .descriptorType = vk::DescriptorType::eCombinedImageSampler,
		                .pImageInfo = &source_info,
		        },
		        vk::WriteDescriptorSet{
		                .dstSet = descriptor_sets[4 + view],
		                .dstBinding = 1,
		                .descriptorCount = 1,
		                .descriptorType = vk::DescriptorType::eStorageImage,
		                .pImageInfo = &destination_info,
		        },
		};
		vk.device.updateDescriptorSets(writes, {});
	}

	vk.name(vk::Image(easu_image), "FSR EASU image");
	vk.name(vk::Image(output_image), "FSR RCAS image");
	vk.name(*easu_pipeline, "FSR 1 EASU pipeline");
	vk.name(*rcas_pipeline, "FSR 1 RCAS pipeline");
}

fsr_upscaler::output fsr_upscaler::upscale(vk::raii::Device & device,
                                           vk::raii::CommandBuffer & command,
                                           const std::array<vk::ImageView, 2> & source,
                                           const std::array<xrt_rect, 2> & source_rect,
                                           bool source_is_linear,
                                           uint32_t descriptor_bank)
{
	if (descriptor_bank > 1)
		throw std::out_of_range("FSR descriptor bank");

	output result{
	        .views = {*output_views[0], *output_views[1]},
	};
	for (uint32_t view = 0; view < 2; ++view)
		result.rects[view] = scale_rect(source_rect[view], input_extent, output_extent);

	std::array<vk::ImageMemoryBarrier2, 2> easu_barriers;
	for (uint32_t view = 0; view < 2; ++view)
	{
		easu_barriers[view] = {
		        .srcStageMask = easu_valid ? vk::PipelineStageFlagBits2::eComputeShader : vk::PipelineStageFlagBits2::eNone,
		        .srcAccessMask = easu_valid ? vk::AccessFlagBits2::eShaderSampledRead : vk::AccessFlagBits2::eNone,
		        .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
		        .dstAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
		        .oldLayout = easu_valid ? vk::ImageLayout::eShaderReadOnlyOptimal : vk::ImageLayout::eUndefined,
		        .newLayout = vk::ImageLayout::eGeneral,
		        .image = easu_image,
		        .subresourceRange = {
		                .aspectMask = vk::ImageAspectFlagBits::eColor,
		                .levelCount = 1,
		                .baseArrayLayer = view,
		                .layerCount = 1,
		        },
		};
	}
	command.pipelineBarrier2({.imageMemoryBarrierCount = 2, .pImageMemoryBarriers = easu_barriers.data()});
	command.bindPipeline(vk::PipelineBindPoint::eCompute, *easu_pipeline);

	for (uint32_t view = 0; view < 2; ++view)
	{
		const vk::DescriptorImageInfo source_info{
		        .sampler = *sampler,
		        .imageView = source[view],
		        .imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
		};
		const vk::DescriptorImageInfo destination_info{
		        .imageView = *easu_views[view],
		        .imageLayout = vk::ImageLayout::eGeneral,
		};
		const uint32_t set_index = descriptor_bank * 2 + view;
		const std::array writes{
		        vk::WriteDescriptorSet{
		                .dstSet = descriptor_sets[set_index],
		                .dstBinding = 0,
		                .descriptorCount = 1,
		                .descriptorType = vk::DescriptorType::eCombinedImageSampler,
		                .pImageInfo = &source_info,
		        },
		        vk::WriteDescriptorSet{
		                .dstSet = descriptor_sets[set_index],
		                .dstBinding = 1,
		                .descriptorCount = 1,
		                .descriptorType = vk::DescriptorType::eStorageImage,
		                .pImageInfo = &destination_info,
		        },
		};
		device.updateDescriptorSets(writes, {});

		const auto constants = make_easu_constants(source_rect[view], result.rects[view], input_extent, source_is_linear);
		command.bindDescriptorSets(vk::PipelineBindPoint::eCompute, *easu_layout, 0, descriptor_sets[set_index], {});
		command.pushConstants<easu_push_constants>(*easu_layout, vk::ShaderStageFlagBits::eCompute, 0, constants);
		command.dispatch(divide_round_up(constants.output_size[0], 8), divide_round_up(constants.output_size[1], 8), 1);
	}

	std::array<vk::ImageMemoryBarrier2, 4> rcas_barriers;
	for (uint32_t view = 0; view < 2; ++view)
	{
		rcas_barriers[view] = {
		        .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
		        .srcAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
		        .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
		        .dstAccessMask = vk::AccessFlagBits2::eShaderSampledRead,
		        .oldLayout = vk::ImageLayout::eGeneral,
		        .newLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
		        .image = easu_image,
		        .subresourceRange = {
		                .aspectMask = vk::ImageAspectFlagBits::eColor,
		                .levelCount = 1,
		                .baseArrayLayer = view,
		                .layerCount = 1,
		        },
		};
		rcas_barriers[2 + view] = {
		        .srcStageMask = output_valid ? vk::PipelineStageFlagBits2::eComputeShader : vk::PipelineStageFlagBits2::eNone,
		        .srcAccessMask = output_valid ? vk::AccessFlagBits2::eShaderSampledRead : vk::AccessFlagBits2::eNone,
		        .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
		        .dstAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
		        .oldLayout = output_valid ? vk::ImageLayout::eShaderReadOnlyOptimal : vk::ImageLayout::eUndefined,
		        .newLayout = vk::ImageLayout::eGeneral,
		        .image = output_image,
		        .subresourceRange = {
		                .aspectMask = vk::ImageAspectFlagBits::eColor,
		                .levelCount = 1,
		                .baseArrayLayer = view,
		                .layerCount = 1,
		        },
		};
	}
	command.pipelineBarrier2({.imageMemoryBarrierCount = 4, .pImageMemoryBarriers = rcas_barriers.data()});
	command.bindPipeline(vk::PipelineBindPoint::eCompute, *rcas_pipeline);

	for (uint32_t view = 0; view < 2; ++view)
	{
		const rcas_push_constants constants{
		        .output_offset = {result.rects[view].offset.w, result.rects[view].offset.h},
		        .output_size = {uint32_t(result.rects[view].extent.w), uint32_t(result.rects[view].extent.h)},
		        .sharpness = std::exp2(-fsr_rcas_sharpness_stops()),
		};
		command.bindDescriptorSets(vk::PipelineBindPoint::eCompute, *rcas_layout, 0, descriptor_sets[4 + view], {});
		command.pushConstants<rcas_push_constants>(*rcas_layout, vk::ShaderStageFlagBits::eCompute, 0, constants);
		command.dispatch(divide_round_up(constants.output_size[0], 8), divide_round_up(constants.output_size[1], 8), 1);
	}

	std::array<vk::ImageMemoryBarrier2, 2> output_barriers;
	for (uint32_t view = 0; view < 2; ++view)
	{
		output_barriers[view] = {
		        .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
		        .srcAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
		        .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
		        .dstAccessMask = vk::AccessFlagBits2::eShaderSampledRead,
		        .oldLayout = vk::ImageLayout::eGeneral,
		        .newLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
		        .image = output_image,
		        .subresourceRange = {
		                .aspectMask = vk::ImageAspectFlagBits::eColor,
		                .levelCount = 1,
		                .baseArrayLayer = view,
		                .layerCount = 1,
		        },
		};
	}
	command.pipelineBarrier2({.imageMemoryBarrierCount = 2, .pImageMemoryBarriers = output_barriers.data()});
	easu_valid = true;
	output_valid = true;
	return result;
}
} // namespace wivrn
