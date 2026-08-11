/*
 * WiVRn VR streaming
 * Copyright (C) 2026 WiVRn-fsr contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "stream_upscaler.h"

#include "application.h"
#include "nis/NIS_Config.h"
#include "spatial_upscaling.h"
#include "vk/pipeline.h"
#include "vk/shader.h"
#include "vk/specialization_constants.h"

#include <array>
#include <cstdlib>
#include <cstring>
#include <spdlog/spdlog.h>
#include <stdexcept>
#include <vk_mem_alloc.h>

namespace
{
uint32_t shader_algorithm(wivrn::from_headset::spatial_upscaling_mode mode)
{
	using enum wivrn::from_headset::spatial_upscaling_mode;
	switch (mode)
	{
		case sgsr1_headset:
			return 1;
		case fsr1_headset:
			return 2;
		case nis_headset:
			return 5;
		default:
			return 0;
	}
}

vk::Extent2D align_extent(vk::Extent2D extent, uint32_t width, uint32_t height)
{
	return {
	        (extent.width + width - 1) / width * width,
	        (extent.height + height - 1) / height * height,
	};
}

vk::raii::ImageView make_layer_view(vk::raii::Device & device,
                                    vk::Image image,
                                    vk::Format format,
                                    uint32_t layer)
{
	return device.createImageView({
	        .image = image,
	        .viewType = vk::ImageViewType::e2D,
	        .format = format,
	        .subresourceRange = {
	                .aspectMask = vk::ImageAspectFlagBits::eColor,
	                .levelCount = 1,
	                .baseArrayLayer = layer,
	                .layerCount = 1,
	        },
	});
}
} // namespace

vk::raii::Pipeline stream_upscaler::make_pipeline(uint32_t algorithm)
{
	auto vertex_shader = load_shader(device, "spatial_upscale.vert");
	auto fragment_shader = load_shader(device, "spatial_upscale.frag");
	auto specialization = make_specialization_constants(int32_t(algorithm));
	vk::pipeline_builder builder{
	        .Stages = {
	                {.stage = vk::ShaderStageFlagBits::eVertex, .module = *vertex_shader, .pName = "main"},
	                {.stage = vk::ShaderStageFlagBits::eFragment, .module = *fragment_shader, .pName = "main", .pSpecializationInfo = specialization},
	        },
	        .InputAssemblyState = {{.topology = vk::PrimitiveTopology::eTriangleList}},
	        .Viewports = {{}},
	        .Scissors = {{}},
	        .RasterizationState = {{.polygonMode = vk::PolygonMode::eFill, .lineWidth = 1}},
	        .MultisampleState = {{.rasterizationSamples = vk::SampleCountFlagBits::e1}},
	        .ColorBlendState = {},
	        .ColorBlendAttachments = {{
	                .colorWriteMask = vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG | vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA,
	        }},
	        .DynamicStates = {vk::DynamicState::eViewport, vk::DynamicState::eScissor},
	        .layout = *pipeline_layout,
	        .renderPass = *renderpass,
	};
	return device.createGraphicsPipeline(application::get_pipeline_cache(), builder);
}

void stream_upscaler::initialize_nis()
{
	// NVScaler dispatches 32x24 output blocks. Pad the private image instead of
	// the OpenXR swapchain so edge workgroups remain in bounds for arbitrary
	// headset resolutions.
	nis_extent = align_extent(output_extent, 32, 24);
	nis_image.emplace(
	        device,
	        vk::ImageCreateInfo{
	                .imageType = vk::ImageType::e2D,
	                .format = vk::Format::eR16G16B16A16Sfloat,
	                .extent = {nis_extent.width, nis_extent.height, 1},
	                .mipLevels = 1,
	                .arrayLayers = view_count,
	                .samples = vk::SampleCountFlagBits::e1,
	                .usage = vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eStorage,
	        },
	        VmaAllocationCreateInfo{.usage = VMA_MEMORY_USAGE_AUTO},
	        "headset NIS output");
	nis_views.reserve(view_count);
	for (uint32_t view = 0; view < view_count; ++view)
		nis_views.emplace_back(make_layer_view(device,
		                                       vk::Image(*nis_image),
		                                       vk::Format::eR16G16B16A16Sfloat,
		                                       view));

	NISConfig config{};
	if (!NVScalerUpdateConfig(config,
	                          0.5f,
	                          0,
	                          0,
	                          input_extent.width,
	                          input_extent.height,
	                          input_extent.width,
	                          input_extent.height,
	                          0,
	                          0,
	                          output_extent.width,
	                          output_extent.height,
	                          nis_extent.width,
	                          nis_extent.height,
	                          NISHDRMode::None))
		throw std::runtime_error("NIS rejected the requested scale");

	const VmaAllocationCreateInfo host_allocation{
	        .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT,
	        .usage = VMA_MEMORY_USAGE_AUTO,
	};
	nis_config_buffer.emplace(
	        device,
	        vk::BufferCreateInfo{
	                .size = sizeof(config),
	                .usage = vk::BufferUsageFlagBits::eUniformBuffer,
	        },
	        host_allocation,
	        "headset NIS configuration");
	std::memcpy(nis_config_buffer->data(), &config, sizeof(config));
	vmaFlushAllocation(vk_allocator::instance(), VmaAllocation(*nis_config_buffer), 0, VK_WHOLE_SIZE);
	nis_config_buffer->unmap();

	std::array<float, 64 * 12> coefficients{};
	for (size_t phase = 0; phase < 64; ++phase)
	{
		for (size_t tap = 0; tap < 6; ++tap)
		{
			coefficients[phase * 12 + tap] = coef_scale[phase][tap];
			coefficients[phase * 12 + 6 + tap] = coef_usm[phase][tap];
		}
	}
	nis_coefficient_buffer.emplace(
	        device,
	        vk::BufferCreateInfo{
	                .size = sizeof(coefficients),
	                .usage = vk::BufferUsageFlagBits::eStorageBuffer,
	        },
	        host_allocation,
	        "headset NIS coefficients");
	std::memcpy(nis_coefficient_buffer->data(), coefficients.data(), sizeof(coefficients));
	vmaFlushAllocation(vk_allocator::instance(), VmaAllocation(*nis_coefficient_buffer), 0, VK_WHOLE_SIZE);
	nis_coefficient_buffer->unmap();

	const std::array bindings{
	        vk::DescriptorSetLayoutBinding{0, vk::DescriptorType::eUniformBuffer, 1, vk::ShaderStageFlagBits::eCompute},
	        vk::DescriptorSetLayoutBinding{1, vk::DescriptorType::eSampler, 1, vk::ShaderStageFlagBits::eCompute},
	        vk::DescriptorSetLayoutBinding{2, vk::DescriptorType::eSampledImage, 1, vk::ShaderStageFlagBits::eCompute},
	        vk::DescriptorSetLayoutBinding{3, vk::DescriptorType::eStorageImage, 1, vk::ShaderStageFlagBits::eCompute},
	        vk::DescriptorSetLayoutBinding{4, vk::DescriptorType::eStorageBuffer, 1, vk::ShaderStageFlagBits::eCompute},
	};
	nis_descriptor_set_layout = device.createDescriptorSetLayout({
	        .bindingCount = bindings.size(),
	        .pBindings = bindings.data(),
	});
	const std::array pool_sizes{
	        vk::DescriptorPoolSize{vk::DescriptorType::eUniformBuffer, view_count},
	        vk::DescriptorPoolSize{vk::DescriptorType::eSampler, view_count},
	        vk::DescriptorPoolSize{vk::DescriptorType::eSampledImage, view_count},
	        vk::DescriptorPoolSize{vk::DescriptorType::eStorageImage, view_count},
	        vk::DescriptorPoolSize{vk::DescriptorType::eStorageBuffer, view_count},
	};
	nis_descriptor_pool = device.createDescriptorPool({
	        .maxSets = view_count,
	        .poolSizeCount = pool_sizes.size(),
	        .pPoolSizes = pool_sizes.data(),
	});
	std::array<vk::DescriptorSetLayout, view_count> layouts;
	layouts.fill(*nis_descriptor_set_layout);
	auto allocated_sets = device.allocateDescriptorSets({
	        .descriptorPool = *nis_descriptor_pool,
	        .descriptorSetCount = view_count,
	        .pSetLayouts = layouts.data(),
	});
	for (uint32_t view = 0; view < view_count; ++view)
		nis_descriptor_sets[view] = allocated_sets[view].release();

	for (uint32_t view = 0; view < view_count; ++view)
	{
		const vk::DescriptorBufferInfo config_info{vk::Buffer(*nis_config_buffer), 0, sizeof(config)};
		const vk::DescriptorBufferInfo coefficient_info{vk::Buffer(*nis_coefficient_buffer), 0, sizeof(coefficients)};
		const vk::DescriptorImageInfo sampler_info{*sampler, {}, vk::ImageLayout::eUndefined};
		const vk::DescriptorImageInfo input_info{{}, *input_views[view], vk::ImageLayout::eShaderReadOnlyOptimal};
		const vk::DescriptorImageInfo output_info{{}, *nis_views[view], vk::ImageLayout::eGeneral};
		const std::array writes{
		        vk::WriteDescriptorSet{
		                .dstSet = nis_descriptor_sets[view],
		                .dstBinding = 0,
		                .descriptorCount = 1,
		                .descriptorType = vk::DescriptorType::eUniformBuffer,
		                .pBufferInfo = &config_info,
		        },
		        vk::WriteDescriptorSet{
		                .dstSet = nis_descriptor_sets[view],
		                .dstBinding = 1,
		                .descriptorCount = 1,
		                .descriptorType = vk::DescriptorType::eSampler,
		                .pImageInfo = &sampler_info,
		        },
		        vk::WriteDescriptorSet{
		                .dstSet = nis_descriptor_sets[view],
		                .dstBinding = 2,
		                .descriptorCount = 1,
		                .descriptorType = vk::DescriptorType::eSampledImage,
		                .pImageInfo = &input_info,
		        },
		        vk::WriteDescriptorSet{
		                .dstSet = nis_descriptor_sets[view],
		                .dstBinding = 3,
		                .descriptorCount = 1,
		                .descriptorType = vk::DescriptorType::eStorageImage,
		                .pImageInfo = &output_info,
		        },
		        vk::WriteDescriptorSet{
		                .dstSet = nis_descriptor_sets[view],
		                .dstBinding = 4,
		                .descriptorCount = 1,
		                .descriptorType = vk::DescriptorType::eStorageBuffer,
		                .pBufferInfo = &coefficient_info,
		        },
		};
		device.updateDescriptorSets(writes, {});

		const vk::DescriptorImageInfo final_info{
		        .sampler = *sampler,
		        .imageView = *nis_views[view],
		        .imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
		};
		device.updateDescriptorSets(
		        vk::WriteDescriptorSet{
		                .dstSet = descriptor_sets[view_count + view],
		                .dstBinding = 0,
		                .descriptorCount = 1,
		                .descriptorType = vk::DescriptorType::eCombinedImageSampler,
		                .pImageInfo = &final_info,
		        },
		        {});
	}

	nis_pipeline_layout = device.createPipelineLayout({
	        .setLayoutCount = 1,
	        .pSetLayouts = &*nis_descriptor_set_layout,
	});
	auto compute_shader = load_shader(device, "nis.comp");
	nis_pipeline = device.createComputePipeline(
	        application::get_pipeline_cache(),
	        vk::ComputePipelineCreateInfo{
	                .stage = {
	                        .stage = vk::ShaderStageFlagBits::eCompute,
	                        .module = *compute_shader,
	                        .pName = "main",
	                },
	                .layout = *nis_pipeline_layout,
	        });
}

stream_upscaler::stream_upscaler(vk::raii::Device & device,
                                 const std::vector<vk::Image> & output_images,
                                 vk::Extent2D input_extent,
                                 vk::Extent2D output_extent,
                                 vk::Format format,
                                 wivrn::from_headset::spatial_upscaling_mode mode) :
        device(device),
        requested_mode(mode),
        active_mode(mode),
        input_extent(input_extent),
        output_extent(output_extent),
        input_image(
                device,
                vk::ImageCreateInfo{
                        .imageType = vk::ImageType::e2D,
                        .format = format,
                        .extent = {input_extent.width, input_extent.height, 1},
                        .mipLevels = 1,
                        .arrayLayers = view_count,
                        .samples = vk::SampleCountFlagBits::e1,
                        .usage = vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eSampled,
                },
                VmaAllocationCreateInfo{.usage = VMA_MEMORY_USAGE_AUTO},
                "headset upscaler input"),
        input_views{
                make_layer_view(device, input_image, format, 0),
                make_layer_view(device, input_image, format, 1),
        }
{
	if (!wivrn::valid_upscaling_extent(mode,
	                                   {input_extent.width, input_extent.height},
	                                   {output_extent.width, output_extent.height}))
		throw std::invalid_argument("invalid headset upscaling dimensions");

	sampler = device.createSampler({
	        .magFilter = vk::Filter::eLinear,
	        .minFilter = vk::Filter::eLinear,
	        .mipmapMode = vk::SamplerMipmapMode::eNearest,
	        .addressModeU = vk::SamplerAddressMode::eClampToEdge,
	        .addressModeV = vk::SamplerAddressMode::eClampToEdge,
	        .addressModeW = vk::SamplerAddressMode::eClampToEdge,
	});

	vk::AttachmentDescription attachment{
	        .format = format,
	        .samples = vk::SampleCountFlagBits::e1,
	        .loadOp = vk::AttachmentLoadOp::eClear,
	        .storeOp = vk::AttachmentStoreOp::eStore,
	        .initialLayout = vk::ImageLayout::eUndefined,
	        .finalLayout = vk::ImageLayout::eColorAttachmentOptimal,
	};
	vk::AttachmentReference color_ref{.attachment = 0, .layout = vk::ImageLayout::eColorAttachmentOptimal};
	vk::SubpassDescription subpass{
	        .pipelineBindPoint = vk::PipelineBindPoint::eGraphics,
	        .colorAttachmentCount = 1,
	        .pColorAttachments = &color_ref,
	};
	renderpass = device.createRenderPass({
	        .attachmentCount = 1,
	        .pAttachments = &attachment,
	        .subpassCount = 1,
	        .pSubpasses = &subpass,
	});

	vk::DescriptorSetLayoutBinding binding{
	        .binding = 0,
	        .descriptorType = vk::DescriptorType::eCombinedImageSampler,
	        .descriptorCount = 1,
	        .stageFlags = vk::ShaderStageFlagBits::eFragment,
	};
	descriptor_set_layout = device.createDescriptorSetLayout({.bindingCount = 1, .pBindings = &binding});
	vk::DescriptorPoolSize pool_size{.type = vk::DescriptorType::eCombinedImageSampler, .descriptorCount = view_count * 2};
	descriptor_pool = device.createDescriptorPool({
	        .maxSets = view_count * 2,
	        .poolSizeCount = 1,
	        .pPoolSizes = &pool_size,
	});
	std::array<vk::DescriptorSetLayout, view_count * 2> layouts;
	layouts.fill(*descriptor_set_layout);
	auto sets = device.allocateDescriptorSets({
	        .descriptorPool = *descriptor_pool,
	        .descriptorSetCount = view_count * 2,
	        .pSetLayouts = layouts.data(),
	});
	for (uint32_t view = 0; view < descriptor_sets.size(); ++view)
		descriptor_sets[view] = sets[view].release();
	for (uint32_t view = 0; view < view_count; ++view)
	{
		vk::DescriptorImageInfo image_info{
		        .sampler = *sampler,
		        .imageView = *input_views[view],
		        .imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
		};
		vk::WriteDescriptorSet write{
		        .dstSet = descriptor_sets[view],
		        .dstBinding = 0,
		        .descriptorCount = 1,
		        .descriptorType = vk::DescriptorType::eCombinedImageSampler,
		        .pImageInfo = &image_info,
		};
		device.updateDescriptorSets(write, {});
	}

	struct alignas(16) push_constants
	{
		std::array<float, 4> viewport;
		std::array<float, 4> output;
		float sharpness;
		uint32_t premultiply_alpha;
	};
	vk::PushConstantRange range{
	        .stageFlags = vk::ShaderStageFlagBits::eFragment,
	        .size = sizeof(push_constants),
	};
	pipeline_layout = device.createPipelineLayout({
	        .setLayoutCount = 1,
	        .pSetLayouts = &*descriptor_set_layout,
	        .pushConstantRangeCount = 1,
	        .pPushConstantRanges = &range,
	});

	// Always create the initialized bilinear path first.
	bilinear_pipeline = make_pipeline(0);
	try
	{
		if (std::getenv("WIVRN_SPATIAL_FORCE_FALLBACK"))
			throw std::runtime_error("fallback requested by WIVRN_SPATIAL_FORCE_FALLBACK");
		algorithm_pipeline = make_pipeline(shader_algorithm(mode));
		if (mode == wivrn::from_headset::spatial_upscaling_mode::fsr1_headset)
		{
			fsr_rcas_pipeline = make_pipeline(4);
			fsr_image.emplace(
			        device,
			        vk::ImageCreateInfo{
			                .imageType = vk::ImageType::e2D,
			                .format = format,
			                .extent = {output_extent.width, output_extent.height, 1},
			                .mipLevels = 1,
			                .arrayLayers = view_count,
			                .samples = vk::SampleCountFlagBits::e1,
			                .usage = vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eSampled,
			        },
			        VmaAllocationCreateInfo{.usage = VMA_MEMORY_USAGE_AUTO},
			        "headset FSR1 EASU output");
			fsr_views.reserve(view_count);
			fsr_framebuffers.reserve(view_count);
			for (uint32_t view = 0; view < view_count; ++view)
			{
				fsr_views.emplace_back(make_layer_view(device, vk::Image(*fsr_image), format, view));
				fsr_framebuffers.emplace_back(device.createFramebuffer({
				        .renderPass = *renderpass,
				        .attachmentCount = 1,
				        .pAttachments = &*fsr_views.back(),
				        .width = output_extent.width,
				        .height = output_extent.height,
				        .layers = 1,
				}));
				vk::DescriptorImageInfo image_info{
				        .sampler = *sampler,
				        .imageView = *fsr_views[view],
				        .imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
				};
				vk::WriteDescriptorSet write{
				        .dstSet = descriptor_sets[view_count + view],
				        .dstBinding = 0,
				        .descriptorCount = 1,
				        .descriptorType = vk::DescriptorType::eCombinedImageSampler,
				        .pImageInfo = &image_info,
				};
				device.updateDescriptorSets(write, {});
			}
		}
		else if (mode == wivrn::from_headset::spatial_upscaling_mode::nis_headset)
		{
			initialize_nis();
		}
	}
	catch (const std::exception & error)
	{
		active_mode = wivrn::from_headset::spatial_upscaling_mode::off;
		spdlog::error("Cannot create {} headset upscaler pipeline: {}; using bilinear fallback",
		              wivrn::spatial_upscaling_name(mode),
		              error.what());
	}

	output_views.reserve(output_images.size() * view_count);
	framebuffers.reserve(output_images.size() * view_count);
	for (auto image: output_images)
	{
		for (uint32_t view = 0; view < view_count; ++view)
		{
			output_views.emplace_back(make_layer_view(device, image, format, view));
			framebuffers.emplace_back(device.createFramebuffer({
			        .renderPass = *renderpass,
			        .attachmentCount = 1,
			        .pAttachments = &*output_views.back(),
			        .width = output_extent.width,
			        .height = output_extent.height,
			        .layers = 1,
			}));
		}
	}
}

std::vector<vk::Image> stream_upscaler::input_images() const
{
	return {vk::Image(input_image)};
}

void stream_upscaler::upscale(vk::raii::CommandBuffer & command_buffer,
                              int destination,
                              bool premultiply_alpha)
{
	if (destination < 0 || destination * int(view_count) >= int(framebuffers.size()))
		throw std::runtime_error("invalid headset upscaler destination");

	vk::ImageMemoryBarrier barrier{
	        .srcAccessMask = vk::AccessFlagBits::eColorAttachmentWrite,
	        .dstAccessMask = vk::AccessFlagBits::eShaderRead,
	        .oldLayout = vk::ImageLayout::eColorAttachmentOptimal,
	        .newLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
	        .image = input_image,
	        .subresourceRange = {
	                .aspectMask = vk::ImageAspectFlagBits::eColor,
	                .levelCount = 1,
	                .layerCount = view_count,
	        },
	};
	const auto input_consumer = active_mode == wivrn::from_headset::spatial_upscaling_mode::nis_headset
	                                    ? vk::PipelineStageFlagBits::eComputeShader
	                                    : vk::PipelineStageFlagBits::eFragmentShader;
	command_buffer.pipelineBarrier(vk::PipelineStageFlagBits::eColorAttachmentOutput,
	                               input_consumer,
	                               {},
	                               {},
	                               {},
	                               barrier);

	struct alignas(16) push_constants
	{
		std::array<float, 4> viewport;
		std::array<float, 4> output;
		float sharpness;
		uint32_t premultiply_alpha;
	};
	const push_constants constants{
	        .viewport = {1.0f / input_extent.width, 1.0f / input_extent.height, float(input_extent.width), float(input_extent.height)},
	        .output = {1.0f / output_extent.width, 1.0f / output_extent.height, float(output_extent.width), float(output_extent.height)},
	        .sharpness = 0.2f,
	        .premultiply_alpha = premultiply_alpha,
	};
	const vk::ClearValue clear{vk::ClearColorValue{std::array{0.0f, 0.0f, 0.0f, 0.0f}}};
	auto draw = [&](uint32_t view,
	                vk::Framebuffer framebuffer,
	                vk::Pipeline pipeline,
	                vk::DescriptorSet descriptor_set,
	                const push_constants & draw_constants) {
		vk::RenderPassBeginInfo begin{
		        .renderPass = *renderpass,
		        .framebuffer = framebuffer,
		        .renderArea = {.extent = output_extent},
		        .clearValueCount = 1,
		        .pClearValues = &clear,
		};
		command_buffer.beginRenderPass(begin, vk::SubpassContents::eInline);
		command_buffer.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline);
		command_buffer.bindDescriptorSets(vk::PipelineBindPoint::eGraphics,
		                                  *pipeline_layout,
		                                  0,
		                                  descriptor_set,
		                                  {});
		command_buffer.pushConstants<push_constants>(*pipeline_layout,
		                                             vk::ShaderStageFlagBits::eFragment,
		                                             0,
		                                             draw_constants);
		command_buffer.setViewport(0, vk::Viewport{0, 0, float(output_extent.width), float(output_extent.height), 0, 1});
		command_buffer.setScissor(0, vk::Rect2D{{0, 0}, output_extent});
		command_buffer.draw(3, 1, 0, 0);
		command_buffer.endRenderPass();
	};

	if (active_mode == wivrn::from_headset::spatial_upscaling_mode::fsr1_headset)
	{
		push_constants easu_constants = constants;
		easu_constants.premultiply_alpha = false;
		for (uint32_t view = 0; view < view_count; ++view)
			draw(view, *fsr_framebuffers[view], *algorithm_pipeline, descriptor_sets[view], easu_constants);
		vk::ImageMemoryBarrier fsr_barrier{
		        .srcAccessMask = vk::AccessFlagBits::eColorAttachmentWrite,
		        .dstAccessMask = vk::AccessFlagBits::eShaderRead,
		        .oldLayout = vk::ImageLayout::eColorAttachmentOptimal,
		        .newLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
		        .image = vk::Image(*fsr_image),
		        .subresourceRange = {
		                .aspectMask = vk::ImageAspectFlagBits::eColor,
		                .levelCount = 1,
		                .layerCount = view_count,
		        },
		};
		command_buffer.pipelineBarrier(vk::PipelineStageFlagBits::eColorAttachmentOutput,
		                               vk::PipelineStageFlagBits::eFragmentShader,
		                               {},
		                               {},
		                               {},
		                               fsr_barrier);
		push_constants rcas_constants = constants;
		rcas_constants.viewport = constants.output;
		for (uint32_t view = 0; view < view_count; ++view)
			draw(view,
			     *framebuffers[destination * view_count + view],
			     *fsr_rcas_pipeline,
			     descriptor_sets[view_count + view],
			     rcas_constants);
	}
	else if (active_mode == wivrn::from_headset::spatial_upscaling_mode::nis_headset)
	{
		vk::ImageMemoryBarrier output_to_compute{
		        .srcAccessMask = nis_initialized ? vk::AccessFlagBits::eShaderRead : vk::AccessFlagBits::eNone,
		        .dstAccessMask = vk::AccessFlagBits::eShaderWrite,
		        .oldLayout = nis_initialized ? vk::ImageLayout::eShaderReadOnlyOptimal : vk::ImageLayout::eUndefined,
		        .newLayout = vk::ImageLayout::eGeneral,
		        .image = vk::Image(*nis_image),
		        .subresourceRange = {
		                .aspectMask = vk::ImageAspectFlagBits::eColor,
		                .levelCount = 1,
		                .layerCount = view_count,
		        },
		};
		command_buffer.pipelineBarrier(nis_initialized ? vk::PipelineStageFlagBits::eFragmentShader
		                                               : vk::PipelineStageFlagBits::eTopOfPipe,
		                               vk::PipelineStageFlagBits::eComputeShader,
		                               {},
		                               {},
		                               {},
		                               output_to_compute);
		nis_initialized = true;
		command_buffer.bindPipeline(vk::PipelineBindPoint::eCompute, *nis_pipeline);
		for (uint32_t view = 0; view < view_count; ++view)
		{
			command_buffer.bindDescriptorSets(vk::PipelineBindPoint::eCompute,
			                                  *nis_pipeline_layout,
			                                  0,
			                                  nis_descriptor_sets[view],
			                                  {});
			command_buffer.dispatch(nis_extent.width / 32, nis_extent.height / 24, 1);
		}
		vk::ImageMemoryBarrier output_to_fragment{
		        .srcAccessMask = vk::AccessFlagBits::eShaderWrite,
		        .dstAccessMask = vk::AccessFlagBits::eShaderRead,
		        .oldLayout = vk::ImageLayout::eGeneral,
		        .newLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
		        .image = vk::Image(*nis_image),
		        .subresourceRange = {
		                .aspectMask = vk::ImageAspectFlagBits::eColor,
		                .levelCount = 1,
		                .layerCount = view_count,
		        },
		};
		command_buffer.pipelineBarrier(vk::PipelineStageFlagBits::eComputeShader,
		                               vk::PipelineStageFlagBits::eFragmentShader,
		                               {},
		                               {},
		                               {},
		                               output_to_fragment);
		push_constants copy_constants = constants;
		copy_constants.viewport = {
		        1.0f / nis_extent.width,
		        1.0f / nis_extent.height,
		        float(output_extent.width),
		        float(output_extent.height),
		};
		for (uint32_t view = 0; view < view_count; ++view)
			draw(view,
			     *framebuffers[destination * view_count + view],
			     *algorithm_pipeline,
			     descriptor_sets[view_count + view],
			     copy_constants);
	}
	else
	{
		const auto pipeline = active_mode == requested_mode ? *algorithm_pipeline : *bilinear_pipeline;
		for (uint32_t view = 0; view < view_count; ++view)
			draw(view,
			     *framebuffers[destination * view_count + view],
			     pipeline,
			     descriptor_sets[view],
			     constants);
	}
}
