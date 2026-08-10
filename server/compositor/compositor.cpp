/*
 * WiVRn VR streaming
 * Copyright (C) 2026  Patrick Nicolas <patricknicolas@laposte.net>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

// Copyright 2019-2024, Collabora, Ltd.
// Copyright 2025-2026, NVIDIA CORPORATION.

#include "compositor.h"

// Monado includes
#include "driver/xrt_cast.h"
#include "main/comp_frame.h"
#include "util/comp_render_helpers.h"
#include "util/comp_swapchain.h"
#include "util/comp_vulkan.h"
#include "util/u_debug.h"
#include "util/u_handles.h"
#include "util/u_time.h"
#include "vk/vk_helpers.h"

#include "driver/wivrn_session.h"
#include "encoder/video_encoder.h"
#include "inplace_vector.hpp"
#include "utils/method.h"
#include "utils/wivrn_trace.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdlib>

#include "xrt/xrt_config_build.h" // IWYU pragma: keep
#ifdef XRT_FEATURE_RENDERDOC
#include "renderdoc_app.h"

static auto renderdoc()
{
	auto x = []() {
		RENDERDOC_API_1_5_0 * rdoc_api = nullptr;
		const char * env = std::getenv("ENABLE_VULKAN_RENDERDOC_CAPTURE");
		if (not env or env != std::string_view("1"))
			return rdoc_api;
		void * mod = dlopen("librenderdoc.so", RTLD_NOW | RTLD_NOLOAD);
		if (mod)
		{
			pRENDERDOC_GetAPI RENDERDOC_GetAPI = (pRENDERDOC_GetAPI)dlsym(mod, "RENDERDOC_GetAPI");
			XRT_MAYBE_UNUSED int ret = RENDERDOC_GetAPI(eRENDERDOC_API_Version_1_5_0, (void **)&rdoc_api);
			assert(ret == 1);
		}
		return rdoc_api;
	};
	static auto res = x();
	return res;
}
#endif

DEBUG_GET_ONCE_LOG_OPTION(log, "XRT_COMPOSITOR_LOG", U_LOGGING_INFO)
DEBUG_GET_ONCE_BOOL_OPTION(fsr_frame_generation, "WIVRN_FSR_FRAME_GENERATION", false)
DEBUG_GET_ONCE_BOOL_OPTION(fsr_frame_generation_handoff, "WIVRN_FSR_FRAME_GENERATION_HANDOFF", false)
DEBUG_GET_ONCE_BOOL_OPTION(fsr_frame_generation_double_rate, "WIVRN_FSR_FRAME_GENERATION_DOUBLE_RATE", false)
DEBUG_GET_ONCE_BOOL_OPTION(fsr_frame_generation_color_only, "WIVRN_FSR_FRAME_GENERATION_COLOR_ONLY", false)

int frame_generation_search_radius()
{
	static const int radius = [] {
		const char * value = std::getenv("WIVRN_FSR_FRAME_GENERATION_SEARCH_RADIUS");
		if (value == nullptr || *value == '\0')
			return 2;

		char * end = nullptr;
		const long parsed = std::strtol(value, &end, 10);
		if (end == value || *end != '\0')
			return 2;
		return static_cast<int>(std::clamp(parsed, 1L, 4L));
	}();
	return radius;
}

static xrt_swapchain *
frame_synthesis_depth_swapchain(const comp_layer * layer, uint32_t view)
{
	assert(layer->data.type == XRT_LAYER_PROJECTION);
	return layer->sc_array[2 * XRT_MAX_VIEWS + view];
}

namespace details
{
template <auto Method, typename Result, typename... Args>
struct method_trait<Method, Result (wivrn::compositor::*)(Args...)>
{
	static Result magic(xrt_compositor * arg, Args... args)
	{
		return std::invoke(
		        Method,
		        static_cast<wivrn::compositor *>(reinterpret_cast<struct comp_base *>(arg)),
		        args...);
	}
};

} // namespace details

namespace
{
const comp_swapchain_image & get_layer_image(const comp_layer & layer, uint32_t swapchain_index, uint32_t image_index)
{
	return reinterpret_cast<struct comp_swapchain *>(comp_layer_get_swapchain(&layer, swapchain_index))->images[image_index];
}

std::array<vk::Format, 3> image_formats(int bit_depth)
{
	switch (bit_depth)
	{
		case 8:
			return {
			        vk::Format::eR8Unorm,
			        vk::Format::eR8G8Unorm,
			        vk::Format::eG8B8R82Plane420Unorm,
			};
		case 10:
			return {
			        vk::Format::eR16Unorm,
			        vk::Format::eR16G16Unorm,
			        vk::Format::eG10X6B10X6R10X62Plane420Unorm3Pack16,
			};
	}
	throw std::runtime_error(std::format("Unsupported bit depth {}", bit_depth));
}

std::array<wivrn::compositor::image, 4> make_images(wivrn::vk_bundle & vk, vk::CommandPool command_pool, std::span<wivrn::encoder_settings> encoders)
{
	auto formats = image_formats(encoders[0].bit_depth);

	vk::StructureChain image_info{
	        vk::ImageCreateInfo{
	                .flags = vk::ImageCreateFlagBits::eExtendedUsage | vk::ImageCreateFlagBits::eMutableFormat,
	                .imageType = vk::ImageType::e2D,
	                .format = formats.back(),
	                .extent = {
	                        .width = encoders[0].width,
	                        .height = encoders[0].height,
	                        .depth = 1,
	                },
	                .mipLevels = 1,
	                .arrayLayers = 3, // left, right then alpha
	                .samples = vk::SampleCountFlagBits::e1,
	                .usage = vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eTransferSrc,
	        },
	        vk::ImageFormatListCreateInfo{
	                .viewFormatCount = formats.size(),
	                .pViewFormats = formats.data(),
	        },
	};
#if WIVRN_USE_VULKAN_ENCODE
	if (
	        std::get<vk::PhysicalDeviceVideoMaintenance1FeaturesKHR>(vk.feat).videoMaintenance1 and
	        std::ranges::contains(
	                encoders,
	                wivrn::encoder_vulkan,
	                &wivrn::encoder_settings::encoder_name))
	{
		image_info.get().flags |= vk::ImageCreateFlagBits::eVideoProfileIndependentKHR;
		image_info.get().usage |= vk::ImageUsageFlagBits::eVideoEncodeSrcKHR;
	}
#endif

	auto make_image = [&](int i) {
		vk::ImageViewUsageCreateInfo usage{
		        .usage = vk::ImageUsageFlagBits::eStorage,
		};
		image_allocation image{
		        vk.device,
		        image_info.get(),
		        VmaAllocationCreateInfo{.usage = VMA_MEMORY_USAGE_AUTO},
		        std::format("compositor YCbCr image {}", i),
		};
		vk::Image vk_image{image};
		return wivrn::compositor::image{
		        .image{std::move(image)},
		        .view_y{
		                vk.device,
		                {
		                        .pNext = &usage,
		                        .image = vk_image,
		                        .viewType = vk::ImageViewType::e2DArray,
		                        .format = formats[0],
		                        .subresourceRange = {
		                                .aspectMask = vk::ImageAspectFlagBits::ePlane0,
		                                .levelCount = 1,
		                                .layerCount = image_info.get().arrayLayers,
		                        },
		                },
		        },
		        .view_cbcr{
		                vk.device,
		                {
		                        .pNext = &usage,
		                        .image = vk_image,
		                        .viewType = vk::ImageViewType::e2DArray,
		                        .format = formats[1],
		                        .subresourceRange = {
		                                .aspectMask = vk::ImageAspectFlagBits::ePlane1,
		                                .levelCount = 1,
		                                .layerCount = image_info.get().arrayLayers,
		                        },
		                },
		        }};
	};

	return {make_image(0), make_image(1), make_image(2), make_image(3)};
}

vk::raii::Semaphore make_semaphore(wivrn::vk_bundle & vk)
{
	vk::raii::Semaphore res{
	        vk.device,
	        vk::StructureChain{
	                vk::SemaphoreCreateInfo{},
	                vk::SemaphoreTypeCreateInfo{
	                        .semaphoreType = vk::SemaphoreType::eTimeline,
	                },
	        }
	                .get(),
	};
	vk.name(res, "compositor semaphore");
	return res;
}

vk::Extent3D render_extent(const wivrn::from_headset::headset_info_packet & info)
{
	return {
	        .width = info.render_eye_width,
	        .height = info.render_eye_height,
	        .depth = 1,
	};
}

XrPosef interpolate_pose(const XrPosef & a, const XrPosef & b, float t)
{
	XrPosef result{};
	result.position.x = std::lerp(a.position.x, b.position.x, t);
	result.position.y = std::lerp(a.position.y, b.position.y, t);
	result.position.z = std::lerp(a.position.z, b.position.z, t);

	// Spherical interpolation keeps the candidate's projection pose on the
	// same rotation path as the two composed frames. The sign correction avoids
	// taking the long path when the equivalent quaternion representation flips.
	float bx = b.orientation.x;
	float by = b.orientation.y;
	float bz = b.orientation.z;
	float bw = b.orientation.w;
	float dot = a.orientation.x * bx + a.orientation.y * by + a.orientation.z * bz + a.orientation.w * bw;
	if (dot < 0.0f)
	{
		dot = -dot;
		bx = -bx;
		by = -by;
		bz = -bz;
		bw = -bw;
	}

	float x;
	float y;
	float z;
	float w;
	if (dot > 0.9995f)
	{
		x = std::lerp(a.orientation.x, bx, t);
		y = std::lerp(a.orientation.y, by, t);
		z = std::lerp(a.orientation.z, bz, t);
		w = std::lerp(a.orientation.w, bw, t);
	}
	else
	{
		const float angle = std::acos(std::clamp(dot, -1.0f, 1.0f));
		const float sin_angle = std::sin(angle);
		const float weight_a = std::sin((1.0f - t) * angle) / sin_angle;
		const float weight_b = std::sin(t * angle) / sin_angle;
		x = weight_a * a.orientation.x + weight_b * bx;
		y = weight_a * a.orientation.y + weight_b * by;
		z = weight_a * a.orientation.z + weight_b * bz;
		w = weight_a * a.orientation.w + weight_b * bw;
	}

	const float length = std::sqrt(x * x + y * y + z * z + w * w);
	if (length > 1e-6f)
	{
		result.orientation = {
		        .x = x / length,
		        .y = y / length,
		        .z = z / length,
		        .w = w / length,
		};
	}
	else
	{
		result.orientation = a.orientation;
	}
	return result;
}

struct frame_generation_reprojection
{
	std::array<float, 16> transform;
	std::array<float, 4> target_to_tangent;
};

frame_generation_reprojection make_frame_generation_reprojection(const XrPosef & source_pose,
                                                                 const XrFovf & source_fov,
                                                                 const XrPosef & target_pose,
                                                                 const XrFovf & target_fov)
{
	const xrt_pose xrt_source_pose = xrt_cast(source_pose);
	const xrt_fov xrt_source_fov = xrt_cast(source_fov);
	const xrt_pose xrt_target_pose = xrt_cast(target_pose);
	const xrt_fov xrt_target_fov = xrt_cast(target_fov);
	xrt_matrix_4x4 transform{};
	xrt_normalized_rect target_to_tangent{};
	render_calc_time_warp_matrix(&xrt_source_pose, &xrt_source_fov, &xrt_target_pose, &transform);
	render_calc_uv_to_tangent_lengths_rect(&xrt_target_fov, &target_to_tangent);

	frame_generation_reprojection result{};
	std::copy(std::begin(transform.v), std::end(transform.v), result.transform.begin());
	result.target_to_tangent = {
	        target_to_tangent.x,
	        target_to_tangent.y,
	        target_to_tangent.w,
	        target_to_tangent.h,
	};
	return result;
}

vk::raii::ImageView make_frame_history_view(wivrn::vk_bundle & vk, image_allocation & image, uint32_t layer)
{
	return vk::raii::ImageView{
	        vk.device,
	        vk::ImageViewCreateInfo{
	                .image = image,
	                .viewType = vk::ImageViewType::e2D,
	                .format = vk::Format::eR8G8B8A8Srgb,
	                .subresourceRange = {
	                        .aspectMask = vk::ImageAspectFlagBits::eColor,
	                        .baseMipLevel = 0,
	                        .levelCount = 1,
	                        .baseArrayLayer = layer,
	                        .layerCount = 1,
	                },
	        }};
}

struct frame_generation_push_constants
{
	std::array<float, 2> inv_size;
	float synthesis_factor;
	float min_depth;
	float max_depth;
	float pose_reprojection;
	std::array<float, 2> motion_scale;
	std::array<float, 2> motion_offset;
	std::array<float, 2> transform_alignment_padding;
	std::array<float, 16> history_transform;
	std::array<float, 4> target_to_tangent;
};

static_assert(sizeof(frame_generation_push_constants) == 128);
static_assert(offsetof(frame_generation_push_constants, history_transform) == 48);
static_assert(offsetof(frame_generation_push_constants, target_to_tangent) == 112);

struct frame_generation_flow_push_constants
{
	std::array<float, 2> inv_current_size;
	std::array<float, 2> current_per_motion;
	int search_radius;
	int sample_stride;
	std::array<float, 2> transform_alignment_padding;
	std::array<float, 16> history_transform;
	std::array<float, 4> current_to_tangent;
};

static_assert(sizeof(frame_generation_flow_push_constants) == 112);
static_assert(offsetof(frame_generation_flow_push_constants, history_transform) == 32);
static_assert(offsetof(frame_generation_flow_push_constants, current_to_tangent) == 96);

vk::raii::ImageView make_frame_generation_output_view(wivrn::vk_bundle & vk,
                                                      image_allocation & image,
                                                      uint32_t layer)
{
	return vk::raii::ImageView{
	        vk.device,
	        vk::ImageViewCreateInfo{
	                .image = image,
	                .viewType = vk::ImageViewType::e2D,
	                .format = vk::Format::eR16G16B16A16Sfloat,
	                .subresourceRange = {
	                        .aspectMask = vk::ImageAspectFlagBits::eColor,
	                        .baseMipLevel = 0,
	                        .levelCount = 1,
	                        .baseArrayLayer = layer,
	                        .layerCount = 1,
	                },
	        }};
}

vk::raii::ImageView make_frame_generation_motion_view(wivrn::vk_bundle & vk,
                                                      image_allocation & image,
                                                      uint32_t layer)
{
	return vk::raii::ImageView{
	        vk.device,
	        vk::ImageViewCreateInfo{
	                .image = image,
	                .viewType = vk::ImageViewType::e2D,
	                .format = vk::Format::eR16G16B16A16Sfloat,
	                .subresourceRange = {
	                        .aspectMask = vk::ImageAspectFlagBits::eColor,
	                        .baseMipLevel = 0,
	                        .levelCount = 1,
	                        .baseArrayLayer = layer,
	                        .layerCount = 1,
	                },
	        }};
}

vk::raii::Sampler make_frame_generation_sampler(wivrn::vk_bundle & vk)
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

vk::raii::DescriptorSetLayout make_frame_generation_ds_layout(wivrn::vk_bundle & vk)
{
	std::array bindings{
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
	return vk::raii::DescriptorSetLayout{
	        vk.device,
	        vk::DescriptorSetLayoutCreateInfo{
	                .bindingCount = uint32_t(bindings.size()),
	                .pBindings = bindings.data(),
	        }};
}

vk::raii::PipelineLayout make_frame_generation_pipeline_layout(wivrn::vk_bundle & vk,
                                                               vk::DescriptorSetLayout ds_layout)
{
	const vk::PushConstantRange push_constants{
	        .stageFlags = vk::ShaderStageFlagBits::eCompute,
	        .offset = 0,
	        .size = sizeof(frame_generation_push_constants),
	};
	return vk::raii::PipelineLayout{
	        vk.device,
	        vk::PipelineLayoutCreateInfo{
	                .setLayoutCount = 1,
	                .pSetLayouts = &ds_layout,
	                .pushConstantRangeCount = 1,
	                .pPushConstantRanges = &push_constants,
	        }};
}

vk::raii::Pipeline make_frame_generation_pipeline(wivrn::vk_bundle & vk, vk::PipelineLayout layout)
{
	auto shader = vk.load_shader("spacewarp_extrapolate");
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

vk::raii::DescriptorPool make_frame_generation_descriptor_pool(wivrn::vk_bundle & vk)
{
	std::array pool_sizes{
	        vk::DescriptorPoolSize{
	                .type = vk::DescriptorType::eCombinedImageSampler,
	                .descriptorCount = 2 * 3,
	        },
	        vk::DescriptorPoolSize{
	                .type = vk::DescriptorType::eStorageImage,
	                .descriptorCount = 2,
	        },
	};
	return vk::raii::DescriptorPool{
	        vk.device,
	        vk::DescriptorPoolCreateInfo{
	                .maxSets = 2,
	                .poolSizeCount = uint32_t(pool_sizes.size()),
	                .pPoolSizes = pool_sizes.data(),
	        }};
}

vk::raii::DescriptorSetLayout make_frame_generation_flow_ds_layout(wivrn::vk_bundle & vk)
{
	std::array bindings{
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
	return vk::raii::DescriptorSetLayout{
	        vk.device,
	        vk::DescriptorSetLayoutCreateInfo{
	                .bindingCount = uint32_t(bindings.size()),
	                .pBindings = bindings.data(),
	        }};
}

vk::raii::PipelineLayout make_frame_generation_flow_pipeline_layout(wivrn::vk_bundle & vk,
                                                                    vk::DescriptorSetLayout ds_layout)
{
	const vk::PushConstantRange push_constants{
	        .stageFlags = vk::ShaderStageFlagBits::eCompute,
	        .offset = 0,
	        .size = sizeof(frame_generation_flow_push_constants),
	};
	return vk::raii::PipelineLayout{
	        vk.device,
	        vk::PipelineLayoutCreateInfo{
	                .setLayoutCount = 1,
	                .pSetLayouts = &ds_layout,
	                .pushConstantRangeCount = 1,
	                .pPushConstantRanges = &push_constants,
	        }};
}

vk::raii::Pipeline make_frame_generation_flow_pipeline(wivrn::vk_bundle & vk, vk::PipelineLayout layout)
{
	auto shader = vk.load_shader("spacewarp_optical_flow");
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

vk::raii::DescriptorPool make_frame_generation_flow_descriptor_pool(wivrn::vk_bundle & vk)
{
	std::array pool_sizes{
	        vk::DescriptorPoolSize{
	                .type = vk::DescriptorType::eCombinedImageSampler,
	                .descriptorCount = 2 * 2,
	        },
	        vk::DescriptorPoolSize{
	                .type = vk::DescriptorType::eStorageImage,
	                .descriptorCount = 2,
	        },
	};
	return vk::raii::DescriptorPool{
	        vk.device,
	        vk::DescriptorPoolCreateInfo{
	                .maxSets = 2,
	                .poolSizeCount = uint32_t(pool_sizes.size()),
	                .pPoolSizes = pool_sizes.data(),
	        }};
}

} // namespace

namespace wivrn
{

compositor::frame_generation_history::frame_generation_history(vk_bundle & vk, vk::Extent3D extent) :
        image{
                vk.device,
                vk::ImageCreateInfo{
                        .imageType = vk::ImageType::e2D,
                        .format = vk::Format::eR8G8B8A8Srgb,
                        .extent = extent,
                        .mipLevels = 1,
                        .arrayLayers = 2,
                        .samples = vk::SampleCountFlagBits::e1,
                        .usage = vk::ImageUsageFlagBits::eTransferDst |
                                 vk::ImageUsageFlagBits::eTransferSrc |
                                 vk::ImageUsageFlagBits::eSampled,
                },
                VmaAllocationCreateInfo{.usage = VMA_MEMORY_USAGE_AUTO},
                "frame-generation color history"},
        views{
                make_frame_history_view(vk, image, 0),
                make_frame_history_view(vk, image, 1),
        }
{
	vk.name(vk::Image(image), "frame-generation color history image");
	for (auto [i, view]: std::ranges::enumerate_view(views))
		vk.name(*view, std::format("frame-generation color history view {}", i));
}

compositor::frame_generation_backend::frame_generation_backend(vk_bundle & vk, vk::Extent3D extent) :
        output{
                vk.device,
                vk::ImageCreateInfo{
                        .imageType = vk::ImageType::e2D,
                        .format = vk::Format::eR16G16B16A16Sfloat,
                        .extent = extent,
                        .mipLevels = 1,
                        .arrayLayers = 2,
                        .samples = vk::SampleCountFlagBits::e1,
                        .usage = vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled,
                },
                VmaAllocationCreateInfo{.usage = VMA_MEMORY_USAGE_AUTO},
                "frame-generation candidate"},
        sampler(make_frame_generation_sampler(vk)),
        output_views{
                make_frame_generation_output_view(vk, output, 0),
                make_frame_generation_output_view(vk, output, 1),
        },
        ds_layout(make_frame_generation_ds_layout(vk)),
        layout(make_frame_generation_pipeline_layout(vk, *ds_layout)),
        pipeline(make_frame_generation_pipeline(vk, *layout)),
        descriptor_pool(make_frame_generation_descriptor_pool(vk))
{
	std::array layouts = {*ds_layout, *ds_layout};
	auto sets = vk.device.allocateDescriptorSets(vk::DescriptorSetAllocateInfo{
	        .descriptorPool = descriptor_pool,
	        .descriptorSetCount = uint32_t(descriptor_sets.size()),
	        .pSetLayouts = layouts.data(),
	});
	for (auto [i, set]: std::ranges::enumerate_view(sets))
		descriptor_sets[i] = set;
	vk.name(vk::Image(output), "frame-generation candidate image");
	for (auto [i, view]: std::ranges::enumerate_view(output_views))
		vk.name(*view, std::format("frame-generation candidate view {}", i));
	vk.name(*pipeline, "frame-generation candidate pipeline");
}

void compositor::timings::add(float us)
{
	int index = this->index;
	this->index = (this->index + 1) % values.size();
	values[index] = us;
}

compositor::frame_generation_flow::frame_generation_flow(vk_bundle & vk, vk::Extent3D extent) :
        motion{
                vk.device,
                vk::ImageCreateInfo{
                        .imageType = vk::ImageType::e2D,
                        .format = vk::Format::eR16G16B16A16Sfloat,
                        .extent = {
                                .width = std::max(1u, extent.width / 4),
                                .height = std::max(1u, extent.height / 4),
                                .depth = 1,
                        },
                        .mipLevels = 1,
                        .arrayLayers = 2,
                        .samples = vk::SampleCountFlagBits::e1,
                        .usage = vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled,
                },
                VmaAllocationCreateInfo{.usage = VMA_MEMORY_USAGE_AUTO},
                "frame-generation color-only motion"},
        motion_views{
                make_frame_generation_motion_view(vk, motion, 0),
                make_frame_generation_motion_view(vk, motion, 1),
        },
        sampler(make_frame_generation_sampler(vk)),
        ds_layout(make_frame_generation_flow_ds_layout(vk)),
        layout(make_frame_generation_flow_pipeline_layout(vk, *ds_layout)),
        pipeline(make_frame_generation_flow_pipeline(vk, *layout)),
        descriptor_pool(make_frame_generation_flow_descriptor_pool(vk))
{
	std::array layouts = {*ds_layout, *ds_layout};
	auto sets = vk.device.allocateDescriptorSets(vk::DescriptorSetAllocateInfo{
	        .descriptorPool = descriptor_pool,
	        .descriptorSetCount = uint32_t(descriptor_sets.size()),
	        .pSetLayouts = layouts.data(),
	});
	for (auto [i, set]: std::ranges::enumerate_view(sets))
		descriptor_sets[i] = set;
	vk.name(vk::Image(motion), "frame-generation color-only motion image");
	for (auto [i, view]: std::ranges::enumerate_view(motion_views))
		vk.name(*view, std::format("frame-generation color-only motion view {}", i));
	vk.name(*pipeline, "frame-generation color-only motion pipeline");
}

xrt_result_t compositor::predict_frame(int64_t * out_frame_id,
                                       int64_t * out_wake_time_ns,
                                       int64_t * out_predicted_gpu_time_ns,
                                       int64_t * out_predicted_display_time_ns,
                                       int64_t * out_predicted_display_period_ns)
{
	std::lock_guard frame_lock(frame_mutex);
	int64_t frame_id = -1;
	int64_t wake_up_time_ns = 0;
	int64_t present_slop_ns = 0;
	int64_t desired_present_time_ns = 0;
	int64_t predicted_display_time_ns = 0;
	pacer.predict(
	        frame_id,
	        wake_up_time_ns,
	        desired_present_time_ns,
	        present_slop_ns,
	        predicted_display_time_ns);

	frame.waited.id = frame_id;
	frame.waited.desired_present_time_ns = desired_present_time_ns;
	frame.waited.present_slop_ns = present_slop_ns;
	frame.waited.predicted_display_time_ns = predicted_display_time_ns;

	*out_frame_id = frame_id;
	*out_wake_time_ns = wake_up_time_ns;
	*out_predicted_gpu_time_ns = desired_present_time_ns; // Not quite right but close enough.
	*out_predicted_display_time_ns = predicted_display_time_ns;
	*out_predicted_display_period_ns = pacer.get_frame_duration();
	return XRT_SUCCESS;
}

xrt_result_t compositor::mark_frame(int64_t frame_id,
                                    xrt_compositor_frame_point point,
                                    int64_t when_ns)
{
	switch (point)
	{
		case XRT_COMPOSITOR_FRAME_POINT_WOKE:
			session.dump_time("wake_up", frame_id, when_ns);
			return XRT_SUCCESS;
		default:
			assert(false);
	}
	return XRT_ERROR_VULKAN;
}

void compositor::inspect_frame_generation_inputs()
{
	if (!frame_generation_probe.requested)
		return;

	uint32_t projection_layers = 0;
	uint32_t depth_layers = 0;
	uint32_t depth_views = 0;
	uint32_t synthesis_layers = 0;
	uint32_t synthesis_depth_views = 0;
	uint32_t motion_vector_views = 0;
	uint32_t ready_views = 0;
	uint32_t missing_auxiliary_swapchains = 0;
	uint32_t invalid_input_views = 0;
	uint32_t projection_view_count = 0;

	for (uint32_t layer_index = 0; layer_index < layer_accum.layer_count; ++layer_index)
	{
		const auto & layer = layer_accum.layers[layer_index];
		if (layer.data.type == XRT_LAYER_PROJECTION)
		{
			projection_layers++;
			projection_view_count += layer.data.view_count;
			bool has_synthesis = false;
			for (uint32_t view = 0; view < layer.data.view_count; ++view)
			{
				const auto & synthesis = layer.data.proj.v[view].frame_synthesis;
				if (synthesis.valid)
				{
					has_synthesis = true;
					motion_vector_views++;
					if (synthesis.depth.rect.extent.w > 0 && synthesis.depth.rect.extent.h > 0)
						synthesis_depth_views++;

					const bool color_swapchain = comp_layer_get_swapchain(&layer, view) != nullptr;
					const bool motion_swapchain = comp_layer_get_motion_vector_swapchain(&layer, view) != nullptr;
					const bool depth_swapchain = frame_synthesis_depth_swapchain(&layer, view) != nullptr;
					const bool valid_rects = synthesis.motion_vector.rect.extent.w > 0 &&
					                         synthesis.motion_vector.rect.extent.h > 0 &&
					                         synthesis.depth.rect.extent.w > 0 &&
					                         synthesis.depth.rect.extent.h > 0;
					const bool valid_depth_range = std::isfinite(synthesis.min_depth) &&
					                               std::isfinite(synthesis.max_depth) &&
					                               std::isfinite(synthesis.near_z) &&
					                               std::isfinite(synthesis.far_z) &&
					                               synthesis.min_depth >= 0.0f &&
					                               synthesis.min_depth <= synthesis.max_depth &&
					                               synthesis.max_depth <= 1.0f &&
					                               synthesis.near_z != synthesis.far_z;
					if (color_swapchain && motion_swapchain && depth_swapchain && valid_rects && valid_depth_range)
						ready_views++;
					else
					{
						if (!color_swapchain || !motion_swapchain || !depth_swapchain)
							missing_auxiliary_swapchains++;
						if (!valid_rects || !valid_depth_range)
							invalid_input_views++;
					}
				}
			}
			if (has_synthesis)
				synthesis_layers++;
		}
		else if (layer.data.type == XRT_LAYER_PROJECTION_DEPTH)
		{
			depth_layers++;
			depth_views += layer.data.view_count;
		}
	}

	frame_generation_probe.frames++;
	const bool standard_depth_ready = depth_layers > 0 && depth_views > 0;
	const bool metadata_inputs_ready = projection_layers == 1 && projection_view_count == 2 && synthesis_layers == 1 &&
	                                   motion_vector_views == 2 && synthesis_depth_views == 2 && ready_views == 2;
	const bool color_only_inputs_ready = frame_generation_probe.color_only_requested &&
	                                     projection_layers == 1 && projection_view_count == 2 &&
	                                     frame_history.has_value() && frame_history->valid;
	const bool inputs_ready = metadata_inputs_ready || color_only_inputs_ready;
	if (!frame_generation_probe.reported || frame_generation_probe.frames <= 10 ||
	    (!inputs_ready && frame_generation_probe.frames % 600 == 0) ||
	    inputs_ready != frame_generation_probe.last_inputs_ready ||
	    ready_views != frame_generation_probe.last_ready_views)
	{
		U_LOG_IFL_I(log_level, "FSR frame-generation compositor probe: requested=%u color_only=%u frame=%" PRIu64 " layers=%u projection_layers=%u depth_layers=%u depth_views=%u synthesis_layers=%u"
		                       " projection_views=%u synthesis_depth_views=%u motion_vector_views=%u ready_views=%u"
		                       " missing_auxiliary_swapchains=%u invalid_input_views=%u standard_depth_ready=%u inputs_ready=%u"
		                       " history_enabled=%u history_valid=%u history_captures=%" PRIu64 " backend=%s backend_dispatches=%" PRIu64 " handoff=%s fallback=normal-composition-available",
		            frame_generation_probe.requested,
		            frame_generation_probe.color_only_requested,
		            frame_generation_probe.frames,
		            layer_accum.layer_count,
		            projection_layers,
		            depth_layers,
		            depth_views,
		            synthesis_layers,
		            projection_view_count,
		            synthesis_depth_views,
		            motion_vector_views,
		            ready_views,
		            missing_auxiliary_swapchains,
		            invalid_input_views,
		            standard_depth_ready,
		            inputs_ready,
		            frame_generation_probe.history_enabled,
		            frame_history.has_value() && frame_history->valid,
		            frame_generation_probe.history_captures,
		            frame_generation_probe.backend_enabled ? "experimental-candidate" : "none",
		            frame_generation_probe.backend_dispatches,
		            frame_generation_probe.double_rate_enabled ? "double-rate" : (frame_generation_probe.handoff_enabled ? "diagnostic-same-rate" : "disabled"));
		frame_generation_probe.reported = true;
		frame_generation_probe.last_inputs_ready = inputs_ready;
		frame_generation_probe.last_ready_views = ready_views;
	}
}

bool compositor::dispatch_frame_generation_motion(vk::raii::CommandBuffer & command,
                                                  const std::array<vk::ImageView, 2> & current_views,
                                                  const std::array<XrPosef, 2> & current_poses,
                                                  const std::array<XrFovf, 2> & current_fovs)
{
	if (!frame_generation_flow_backend || !frame_history || !frame_history->valid || !previous_view_poses_valid)
		return false;

	auto & flow = *frame_generation_flow_backend;
	const auto current_extent = frame_history->image.info().extent;
	const auto motion_extent = flow.motion.info().extent;
	const vk::ImageMemoryBarrier2 motion_to_compute{
	        .srcStageMask = flow.output_valid ? vk::PipelineStageFlagBits2::eComputeShader
	                                          : vk::PipelineStageFlagBits2::eNone,
	        .srcAccessMask = flow.output_valid ? vk::AccessFlagBits2::eShaderSampledRead
	                                           : vk::AccessFlagBits2::eNone,
	        .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
	        .dstAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
	        .oldLayout = flow.output_valid ? vk::ImageLayout::eShaderReadOnlyOptimal
	                                       : vk::ImageLayout::eUndefined,
	        .newLayout = vk::ImageLayout::eGeneral,
	        .image = flow.motion,
	        .subresourceRange = {
	                .aspectMask = vk::ImageAspectFlagBits::eColor,
	                .levelCount = 1,
	                .layerCount = 2,
	        },
	};
	command.pipelineBarrier2({
	        .imageMemoryBarrierCount = 1,
	        .pImageMemoryBarriers = &motion_to_compute,
	});

	command.bindPipeline(vk::PipelineBindPoint::eCompute, *flow.pipeline);
	for (uint32_t view = 0; view < 2; ++view)
	{
		const auto reprojection = make_frame_generation_reprojection(previous_view_poses[view],
		                                                             previous_view_fovs[view],
		                                                             current_poses[view],
		                                                             current_fovs[view]);
		std::array inputs{
		        vk::DescriptorImageInfo{
		                .sampler = *flow.sampler,
		                .imageView = current_views[view],
		                .imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
		        },
		        vk::DescriptorImageInfo{
		                .sampler = *flow.sampler,
		                .imageView = *frame_history->views[view],
		                .imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
		        },
		        vk::DescriptorImageInfo{
		                .imageView = *flow.motion_views[view],
		                .imageLayout = vk::ImageLayout::eGeneral,
		        },
		};
		std::array writes{
		        vk::WriteDescriptorSet{
		                .dstSet = flow.descriptor_sets[view],
		                .dstBinding = 0,
		                .descriptorCount = 1,
		                .descriptorType = vk::DescriptorType::eCombinedImageSampler,
		                .pImageInfo = &inputs[0],
		        },
		        vk::WriteDescriptorSet{
		                .dstSet = flow.descriptor_sets[view],
		                .dstBinding = 1,
		                .descriptorCount = 1,
		                .descriptorType = vk::DescriptorType::eCombinedImageSampler,
		                .pImageInfo = &inputs[1],
		        },
		        vk::WriteDescriptorSet{
		                .dstSet = flow.descriptor_sets[view],
		                .dstBinding = 2,
		                .descriptorCount = 1,
		                .descriptorType = vk::DescriptorType::eStorageImage,
		                .pImageInfo = &inputs[2],
		        },
		};
		vk.device.updateDescriptorSets(writes, {});

		const frame_generation_flow_push_constants constants{
		        .inv_current_size = {
		                1.0f / float(current_extent.width),
		                1.0f / float(current_extent.height),
		        },
		        .current_per_motion = {
		                float(current_extent.width) / float(motion_extent.width),
		                float(current_extent.height) / float(motion_extent.height),
		        },
		        .search_radius = frame_generation_search_radius(),
		        .sample_stride = 4,
		        .history_transform = reprojection.transform,
		        .current_to_tangent = reprojection.target_to_tangent,
		};
		command.bindDescriptorSets(vk::PipelineBindPoint::eCompute,
		                           *flow.layout,
		                           0,
		                           flow.descriptor_sets[view],
		                           {});
		command.pushConstants(*flow.layout,
		                      vk::ShaderStageFlagBits::eCompute,
		                      0,
		                      sizeof(constants),
		                      &constants);
		command.dispatch((motion_extent.width + 15) / 16, (motion_extent.height + 15) / 16, 1);
	}

	const vk::ImageMemoryBarrier2 motion_to_sample{
	        .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
	        .srcAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
	        .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
	        .dstAccessMask = vk::AccessFlagBits2::eShaderSampledRead,
	        .oldLayout = vk::ImageLayout::eGeneral,
	        .newLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
	        .image = flow.motion,
	        .subresourceRange = {
	                .aspectMask = vk::ImageAspectFlagBits::eColor,
	                .levelCount = 1,
	                .layerCount = 2,
	        },
	};
	command.pipelineBarrier2({
	        .imageMemoryBarrierCount = 1,
	        .pImageMemoryBarriers = &motion_to_sample,
	});
	flow.output_valid = true;
	return true;
}

bool compositor::dispatch_frame_generation_candidate(vk::raii::CommandBuffer & command,
                                                     const std::array<vk::ImageView, 2> & current_views,
                                                     const std::array<XrPosef, 2> & current_poses,
                                                     const std::array<XrFovf, 2> & current_fovs,
                                                     const std::array<XrPosef, 2> & candidate_poses,
                                                     const std::array<XrFovf, 2> & candidate_fovs)
{
	if (!frame_generation_probe.backend_enabled || !frame_generation || !frame_history || !frame_history->valid)
		return false;

	const comp_layer * synthesis_layer = nullptr;
	bool color_only = false;
	for (uint32_t layer_index = 0; layer_index < layer_accum.layer_count; ++layer_index)
	{
		const auto & layer = layer_accum.layers[layer_index];
		if (layer.data.type != XRT_LAYER_PROJECTION || layer.data.view_count != 2)
			continue;

		bool ready = true;
		for (uint32_t view = 0; view < 2; ++view)
		{
			const auto & synthesis = layer.data.proj.v[view].frame_synthesis;
			ready = ready && synthesis.valid &&
			        comp_layer_get_motion_vector_swapchain(&layer, view) != nullptr &&
			        frame_synthesis_depth_swapchain(&layer, view) != nullptr &&
			        synthesis.motion_vector.rect.extent.w > 0 &&
			        synthesis.motion_vector.rect.extent.h > 0 &&
			        synthesis.depth.rect.extent.w > 0 &&
			        synthesis.depth.rect.extent.h > 0;
		}
		if (ready)
		{
			synthesis_layer = &layer;
			break;
		}
	}
	if (synthesis_layer == nullptr && frame_generation_probe.color_only_requested)
	{
		if (!frame_generation_flow_backend)
			return false;
		for (uint32_t layer_index = 0; layer_index < layer_accum.layer_count; ++layer_index)
		{
			const auto & layer = layer_accum.layers[layer_index];
			if (layer.data.type == XRT_LAYER_PROJECTION && layer.data.view_count == 2)
			{
				color_only = true;
				break;
			}
		}
	}
	if (synthesis_layer == nullptr && !color_only)
		return false;

	auto get_auxiliary_view = [&](uint32_t view, bool motion) -> vk::ImageView {
		if (color_only)
			return motion ? *frame_generation_flow_backend->motion_views[view] : *frame_history->views[view];
		const auto & synthesis = synthesis_layer->data.proj.v[view].frame_synthesis;
		const auto & sub = motion ? synthesis.motion_vector : synthesis.depth;
		xrt_swapchain * xsc = motion ? comp_layer_get_motion_vector_swapchain(synthesis_layer, view)
		                             : frame_synthesis_depth_swapchain(synthesis_layer, view);
		if (xsc == nullptr)
			return nullptr;
		auto * swapchain = comp_swapchain(xsc);
		if (sub.image_index >= XRT_MAX_SWAPCHAIN_IMAGES)
			return nullptr;
		const auto & image = swapchain->images[sub.image_index];
		if (sub.array_index >= image.array_size)
			return nullptr;
		return get_image_view(&image, synthesis_layer->data.flags, sub.array_index);
	};

	const auto extent = frame_generation->output.info().extent;
	const std::array<float, 2> inv_size{
	        1.0f / float(extent.width),
	        1.0f / float(extent.height),
	};
	std::array<vk::ImageView, 2> motion_views;
	std::array<vk::ImageView, 2> depth_views;
	if (color_only && !dispatch_frame_generation_motion(command, current_views, current_poses, current_fovs))
		return false;
	for (uint32_t view = 0; view < 2; ++view)
	{
		motion_views[view] = get_auxiliary_view(view, true);
		depth_views[view] = get_auxiliary_view(view, false);
		if (!motion_views[view] || !depth_views[view])
			return false;
	}

	const vk::ImageMemoryBarrier2 output_to_compute{
	        .srcStageMask = frame_generation->output_valid ? vk::PipelineStageFlagBits2::eComputeShader
	                                                       : vk::PipelineStageFlagBits2::eNone,
	        .srcAccessMask = frame_generation->output_valid ? vk::AccessFlagBits2::eShaderSampledRead
	                                                        : vk::AccessFlagBits2::eNone,
	        .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
	        .dstAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
	        .oldLayout = frame_generation->output_valid ? vk::ImageLayout::eShaderReadOnlyOptimal
	                                                    : vk::ImageLayout::eUndefined,
	        .newLayout = vk::ImageLayout::eGeneral,
	        .image = frame_generation->output,
	        .subresourceRange = {
	                .aspectMask = vk::ImageAspectFlagBits::eColor,
	                .levelCount = 1,
	                .layerCount = 2,
	        },
	};
	command.pipelineBarrier2({
	        .imageMemoryBarrierCount = 1,
	        .pImageMemoryBarriers = &output_to_compute,
	});

	command.bindPipeline(vk::PipelineBindPoint::eCompute, *frame_generation->pipeline);
	for (uint32_t view = 0; view < 2; ++view)
	{
		const auto * synthesis = synthesis_layer != nullptr ? &synthesis_layer->data.proj.v[view].frame_synthesis : nullptr;
		const auto reprojection = color_only
		                                  ? make_frame_generation_reprojection(previous_view_poses[view],
		                                                                       previous_view_fovs[view],
		                                                                       candidate_poses[view],
		                                                                       candidate_fovs[view])
		                                  : frame_generation_reprojection{};

		std::array inputs{
		        vk::DescriptorImageInfo{
		                .sampler = *frame_generation->sampler,
		                .imageView = *frame_history->views[view],
		                .imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
		        },
		        vk::DescriptorImageInfo{
		                .sampler = *frame_generation->sampler,
		                .imageView = motion_views[view],
		                .imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
		        },
		        vk::DescriptorImageInfo{
		                .sampler = *frame_generation->sampler,
		                .imageView = depth_views[view],
		                .imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
		        },
		};
		vk::DescriptorImageInfo output{
		        .imageView = *frame_generation->output_views[view],
		        .imageLayout = vk::ImageLayout::eGeneral,
		};
		std::array writes{
		        vk::WriteDescriptorSet{
		                .dstSet = frame_generation->descriptor_sets[view],
		                .dstBinding = 0,
		                .descriptorCount = 1,
		                .descriptorType = vk::DescriptorType::eCombinedImageSampler,
		                .pImageInfo = &inputs[0],
		        },
		        vk::WriteDescriptorSet{
		                .dstSet = frame_generation->descriptor_sets[view],
		                .dstBinding = 1,
		                .descriptorCount = 1,
		                .descriptorType = vk::DescriptorType::eCombinedImageSampler,
		                .pImageInfo = &inputs[1],
		        },
		        vk::WriteDescriptorSet{
		                .dstSet = frame_generation->descriptor_sets[view],
		                .dstBinding = 2,
		                .descriptorCount = 1,
		                .descriptorType = vk::DescriptorType::eCombinedImageSampler,
		                .pImageInfo = &inputs[2],
		        },
		        vk::WriteDescriptorSet{
		                .dstSet = frame_generation->descriptor_sets[view],
		                .dstBinding = 3,
		                .descriptorCount = 1,
		                .descriptorType = vk::DescriptorType::eStorageImage,
		                .pImageInfo = &output,
		        },
		};
		vk.device.updateDescriptorSets(writes, {});

		const frame_generation_push_constants constants{
		        .inv_size = inv_size,
		        .synthesis_factor = 0.5f,
		        .min_depth = synthesis != nullptr ? synthesis->min_depth : 0.0f,
		        .max_depth = synthesis != nullptr ? synthesis->max_depth : 1.0f,
		        .pose_reprojection = color_only ? 1.0f : 0.0f,
		        .motion_scale = synthesis != nullptr
		                                ? std::array<float, 2>{synthesis->motion_vector_scale[0], synthesis->motion_vector_scale[1]}
		                                : std::array<float, 2>{-1.0f, -1.0f},
		        .motion_offset = synthesis != nullptr
		                                 ? std::array<float, 2>{synthesis->motion_vector_offset[0], synthesis->motion_vector_offset[1]}
		                                 : std::array<float, 2>{0.0f, 0.0f},
		        .history_transform = reprojection.transform,
		        .target_to_tangent = reprojection.target_to_tangent,
		};
		command.bindDescriptorSets(vk::PipelineBindPoint::eCompute,
		                           *frame_generation->layout,
		                           0,
		                           frame_generation->descriptor_sets[view],
		                           {});
		command.pushConstants(*frame_generation->layout,
		                      vk::ShaderStageFlagBits::eCompute,
		                      0,
		                      sizeof(constants),
		                      &constants);
		command.dispatch((extent.width + 15) / 16, (extent.height + 15) / 16, 1);
	}

	const vk::ImageMemoryBarrier2 output_to_sample{
	        .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
	        .srcAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
	        .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
	        .dstAccessMask = vk::AccessFlagBits2::eShaderSampledRead,
	        .oldLayout = vk::ImageLayout::eGeneral,
	        .newLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
	        .image = frame_generation->output,
	        .subresourceRange = {
	                .aspectMask = vk::ImageAspectFlagBits::eColor,
	                .levelCount = 1,
	                .layerCount = 2,
	        },
	};
	command.pipelineBarrier2({
	        .imageMemoryBarrierCount = 1,
	        .pImageMemoryBarriers = &output_to_sample,
	});
	frame_generation->output_valid = true;
	if (frame_generation_probe.backend_dispatches++ == 0)
		U_LOG_IFL_I(log_level,
		            "FSR frame-generation candidate dispatched: mode=%s views=2 extent=%ux%u factor=0.50 candidate=complete",
		            color_only ? "pose-compensated-color-only" : "metadata",
		            extent.width,
		            extent.height);
	return true;
}

void compositor::capture_frame_generation_history(vk::raii::CommandBuffer & command)
{
	if (!frame_history)
		return;

	const auto extent = frame_history->image.info().extent;
	std::array<vk::ImageMemoryBarrier2, 2> to_copy{
	        vk::ImageMemoryBarrier2{
	                .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
	                .srcAccessMask = vk::AccessFlagBits2::eShaderSampledRead,
	                .dstStageMask = vk::PipelineStageFlagBits2::eTransfer,
	                .dstAccessMask = vk::AccessFlagBits2::eTransferRead,
	                .oldLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
	                .newLayout = vk::ImageLayout::eTransferSrcOptimal,
	                .image = squasher.get_image(),
	                .subresourceRange = {
	                        .aspectMask = vk::ImageAspectFlagBits::eColor,
	                        .levelCount = 1,
	                        .layerCount = 2,
	                },
	        },
	        vk::ImageMemoryBarrier2{
	                .srcStageMask = frame_history->valid ? vk::PipelineStageFlagBits2::eComputeShader : vk::PipelineStageFlagBits2::eNone,
	                .srcAccessMask = frame_history->valid ? vk::AccessFlagBits2::eShaderSampledRead : vk::AccessFlagBits2::eNone,
	                .dstStageMask = vk::PipelineStageFlagBits2::eTransfer,
	                .dstAccessMask = vk::AccessFlagBits2::eTransferWrite,
	                .oldLayout = frame_history->valid ? vk::ImageLayout::eShaderReadOnlyOptimal : vk::ImageLayout::eUndefined,
	                .newLayout = vk::ImageLayout::eTransferDstOptimal,
	                .image = frame_history->image,
	                .subresourceRange = {
	                        .aspectMask = vk::ImageAspectFlagBits::eColor,
	                        .levelCount = 1,
	                        .layerCount = 2,
	                },
	        },
	};
	command.pipelineBarrier2({
	        .imageMemoryBarrierCount = uint32_t(to_copy.size()),
	        .pImageMemoryBarriers = to_copy.data(),
	});

	command.copyImage(
	        squasher.get_image(),
	        vk::ImageLayout::eTransferSrcOptimal,
	        frame_history->image,
	        vk::ImageLayout::eTransferDstOptimal,
	        vk::ImageCopy{
	                .srcSubresource = {
	                        .aspectMask = vk::ImageAspectFlagBits::eColor,
	                        .layerCount = 2,
	                },
	                .dstSubresource = {
	                        .aspectMask = vk::ImageAspectFlagBits::eColor,
	                        .layerCount = 2,
	                },
	                .extent = extent,
	        });

	std::array<vk::ImageMemoryBarrier2, 2> from_copy{
	        vk::ImageMemoryBarrier2{
	                .srcStageMask = vk::PipelineStageFlagBits2::eTransfer,
	                .srcAccessMask = vk::AccessFlagBits2::eTransferRead,
	                .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
	                .dstAccessMask = vk::AccessFlagBits2::eShaderSampledRead,
	                .oldLayout = vk::ImageLayout::eTransferSrcOptimal,
	                .newLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
	                .image = squasher.get_image(),
	                .subresourceRange = {
	                        .aspectMask = vk::ImageAspectFlagBits::eColor,
	                        .levelCount = 1,
	                        .layerCount = 2,
	                },
	        },
	        vk::ImageMemoryBarrier2{
	                .srcStageMask = vk::PipelineStageFlagBits2::eTransfer,
	                .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
	                .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
	                .dstAccessMask = vk::AccessFlagBits2::eShaderSampledRead,
	                .oldLayout = vk::ImageLayout::eTransferDstOptimal,
	                .newLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
	                .image = frame_history->image,
	                .subresourceRange = {
	                        .aspectMask = vk::ImageAspectFlagBits::eColor,
	                        .levelCount = 1,
	                        .layerCount = 2,
	                },
	        },
	};
	command.pipelineBarrier2({
	        .imageMemoryBarrierCount = uint32_t(from_copy.size()),
	        .pImageMemoryBarriers = from_copy.data(),
	});
	frame_history->valid = true;
	if (frame_generation_probe.history_captures++ == 0)
		U_LOG_IFL_I(log_level, "FSR frame-generation persistent color history captured: views=2 extent=%ux%u", extent.width, extent.height);
}

xrt_result_t compositor::layer_commit(xrt_graphics_sync_handle_t sync_handle)
{
	u_graphics_sync_unref(&sync_handle);
	std::lock_guard frame_lock(frame_mutex);

	// Move waited frame to rendering frame, clear waited.
	comp_frame_move_and_clear_locked(&frame.rendering, &frame.waited);
	if (paused.load())
	{
		comp_frame_clear_locked(&frame.rendering);
		return XRT_SUCCESS;
	}
	inspect_frame_generation_inputs();

	U_LOG_IFL_D(log_level, "frame %ld commit %d layers", frame.rendering.id, layer_accum.layer_count);

	if (not session.connected() or not session.get_offset())
	{
		comp_frame_clear_locked(&frame.rendering);
		return XRT_SUCCESS;
	}

	int i = acquire_image();
	if (i < 0)
	{
		comp_frame_clear_locked(&frame.rendering);
		return XRT_SUCCESS;
	}
	int generated_i = -1;
	if (frame_generation_probe.double_rate_enabled)
	{
		generated_i = acquire_image();
		if (generated_i < 0)
		{
			images[i].busy = false;
			comp_frame_clear_locked(&frame.rendering);
			return XRT_SUCCESS;
		}
	}

#ifdef XRT_FEATURE_RENDERDOC
	if (auto r = renderdoc())
		r->StartFrameCapture(NULL, NULL);
#endif

	const bool alpha = layer_accum.data.env_blend_mode == XRT_BLEND_MODE_ALPHA_BLEND;
	const int64_t current_display_time_ns = frame.rendering.predicted_display_time_ns;
	const auto current_display_time = session.get_offset().to_headset(current_display_time_ns);
	auto & view_info = images[i].view_info;
	if (frame_generation_probe.double_rate_enabled)
	{
		// Reserve the even ID for the interval candidate and the odd ID for
		// the application frame tracked by pacer. Deriving both IDs from the
		// OpenXR frame ID keeps feedback correlated across pause/resume.
		images[i].frame_index = uint64_t(frame.rendering.id) * 2 + 1;
	}
	else
		images[i].frame_index = frame.rendering.id;
	view_info = {
	        .display_time = current_display_time,
	        .alpha = alpha,
	};
	auto * generated_view_info = generated_i >= 0 ? &images[generated_i].view_info : nullptr;
	if (generated_view_info)
	{
		images[generated_i].frame_index = uint64_t(frame.rendering.id) * 2;
		*generated_view_info = view_info;
		generated_view_info->display_time = previous_display_time_ns > 0
		                                            ? session.get_offset().to_headset(
		                                                      previous_display_time_ns + (current_display_time_ns - previous_display_time_ns) / 2)
		                                            : current_display_time;
	}
	session.dump_time("begin", frame.rendering.id, os_monotonic_get_ns());

	cmd_pool.reset();
	cmd.begin({.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit});

	cmd.resetQueryPool(*query_pool, 0, 3);
	cmd.writeTimestamp(vk::PipelineStageFlagBits::eTopOfPipe, *query_pool, 0);

	bool flip_y = false;
	std::array<vk::ImageView, 2> src;
	std::array<xrt_rect, 2> src_rect;
	std::array<xrt_fov, 2> src_fov;

	beman::inplace_vector::inplace_vector<vk::ImageMemoryBarrier2, 6> image_barriers;

	// Check if we can pass a layer directly to foveation
	if (!frame_generation_probe.requested and !spatial_upscaler and layer_accum.layer_count == 1 and
	    (layer_accum.layers[0].data.type == XRT_LAYER_PROJECTION or
	     layer_accum.layers[0].data.type == XRT_LAYER_PROJECTION_DEPTH))
	{
		const auto & layer = layer_accum.layers[0];
		for (int view = 0; view < 2; ++view)
		{
			const auto & data = (layer.data.type == XRT_LAYER_PROJECTION ? layer.data.proj.v : layer.data.depth.v)[view];
			src[view] = get_image_view(
			        &get_layer_image(layer, view, data.sub.image_index),
			        layer.data.flags,
			        data.sub.array_index);
			src_rect[view] = data.sub.rect;
			src_fov[view] = data.fov;
			flip_y = layer.data.flip_y;
			view_info.pose[view] = xrt_cast(data.pose);
			view_info.fov[view] = xrt_cast(data.fov);
		}
	}
	else
	{
		// no fast-path, squash layers
		std::array<xrt_pose, 2> poses;
		const auto extent = spatial_upscaler ? render_extent(session.get_info()) : images[0].image.info().extent;
		std::tie(poses, src_fov, src_rect) = squasher.do_layers(
		        vk.device,
		        cmd,
		        session.get_hmd(),
		        pacer.get_frame_duration(),
		        frame.rendering,
		        layer_accum,
		        xrt_rect{.extent{.w = int(extent.width), .h = int(extent.height)}});

		src = squasher.get_views();

		for (int view = 0; view < 2; ++view)
		{
			view_info.pose[view] = xrt_cast(poses[view]);
			view_info.fov[view] = xrt_cast(src_fov[view]);
		}

		image_barriers.push_back(
		        vk::ImageMemoryBarrier2{
		                .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
		                .srcAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
		                .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
		                .dstAccessMask = vk::AccessFlagBits2::eShaderSampledRead,
		                .oldLayout = vk::ImageLayout::eGeneral,
		                .newLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
		                .image = squasher.get_image(),
		                .subresourceRange = {
		                        .aspectMask = vk::ImageAspectFlagBits::eColor,
		                        .levelCount = 1,
		                        .layerCount = 2,
		                },
		        });
	}
	const auto application_poses = view_info.pose;
	const auto application_fovs = view_info.fov;
	const bool have_previous_pose = previous_view_poses_valid && previous_display_time_ns > 0 &&
	                                current_display_time_ns > previous_display_time_ns;
	auto candidate_poses = application_poses;
	const auto candidate_fovs = application_fovs;
	if (have_previous_pose)
	{
		for (uint32_t view = 0; view < 2; ++view)
			candidate_poses[view] = interpolate_pose(previous_view_poses[view], application_poses[view], 0.5f);
	}
	if (generated_view_info)
	{
		generated_view_info->pose = candidate_poses;
		generated_view_info->fov = candidate_fovs;
	}

	image_barriers.push_back(
	        vk::ImageMemoryBarrier2{
	                .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
	                .dstAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
	                .oldLayout = vk::ImageLayout::eUndefined,
	                .newLayout = vk::ImageLayout::eGeneral,
	                .image = images[i].image,
	                .subresourceRange = {
	                        .aspectMask = vk::ImageAspectFlagBits::eColor,
	                        .levelCount = 1,
	                        .layerCount = images[i].image.info().arrayLayers,
	                },
	        });
	if (generated_i >= 0)
	{
		image_barriers.push_back(
		        vk::ImageMemoryBarrier2{
		                .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
		                .dstAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
		                .oldLayout = vk::ImageLayout::eUndefined,
		                .newLayout = vk::ImageLayout::eGeneral,
		                .image = images[generated_i].image,
		                .subresourceRange = {
		                        .aspectMask = vk::ImageAspectFlagBits::eColor,
		                        .levelCount = 1,
		                        .layerCount = images[generated_i].image.info().arrayLayers,
		                },
		        });
	}

	cmd.pipelineBarrier2({
	        .imageMemoryBarrierCount = uint32_t(image_barriers.size()),
	        .pImageMemoryBarriers = image_barriers.data(),
	});
	image_barriers.clear();

	cmd.writeTimestamp(vk::PipelineStageFlagBits::eBottomOfPipe, *query_pool, 1);

	if (session.get_info().eye_gaze)
	{
		auto now = os_monotonic_get_ns();
		session.add_tracking_request(device_id::EYE_GAZE, frame.rendering.desired_present_time_ns, now, now);
	}

	// Dispatch before foveation so the opt-in handoff mode can exercise the
	// complete candidate -> YCbCr -> encoder path. The default path continues
	// to encode the normal composition even when the candidate is available.
	bool candidate_dispatched = false;
	if (frame_generation_probe.last_inputs_ready)
	{
		try
		{
			candidate_dispatched = dispatch_frame_generation_candidate(
			        cmd, src, application_poses, application_fovs, candidate_poses, candidate_fovs);
		}
		catch (const std::exception & e)
		{
			frame_generation_probe.backend_enabled = false;
			frame_generation_probe.handoff_enabled = false;
			U_LOG_IFL_W(log_level,
			            "FSR frame-generation candidate failed: %s; retaining normal composition",
			            e.what());
		}
	}

	auto foveation_src = src;
	if (candidate_dispatched && frame_generation_probe.handoff_enabled)
	{
		foveation_src = {
		        *frame_generation->output_views[0],
		        *frame_generation->output_views[1],
		};
		if (frame_generation_probe.handoff_frames++ == 0)
			U_LOG_IFL_I(log_level,
			            "FSR frame-generation candidate handed off to foveation and encoder; diagnostic %s mode",
			            frame_generation_probe.double_rate_enabled ? "double-rate" : "same-rate");
	}
	if (candidate_dispatched && frame_generation_probe.handoff_enabled && generated_view_info == nullptr && have_previous_pose)
	{
		// Same-rate diagnostic handoff also presents the synthesized midpoint;
		// its layer pose must describe that midpoint rather than the application
		// frame that supplied the current color history.
		view_info.pose = candidate_poses;
	}
	if (generated_view_info)
	{
		if (!candidate_dispatched)
		{
			// The fallback slot contains the current application image rather
			// than a midpoint candidate, so its source pose must match it.
			generated_view_info->pose = application_poses;
			generated_view_info->fov = application_fovs;
		}
		const bool generated_source_is_linear = candidate_dispatched;
		auto generated_src = candidate_dispatched ? std::array<vk::ImageView, 2>{
		                                                          *frame_generation->output_views[0],
		                                                          *frame_generation->output_views[1]}
		                                                : (spatial_upscaler ? squasher.get_unorm_views() : src);
		auto generated_rect = src_rect;
		if (spatial_upscaler)
		{
			auto upscaled = spatial_upscaler->upscale(
			        vk.device, cmd, generated_src, generated_rect, generated_source_is_linear, 0);
			generated_src = upscaled.views;
			generated_rect = upscaled.rects;
		}
		generated_view_info->foveation = foveation.foveate(
		        vk.device,
		        cmd,
		        images[generated_i].view_y,
		        images[generated_i].view_cbcr,
		        flip_y,
		        generated_src,
		        generated_rect,
		        src_fov,
		        generated_view_info->alpha);
		auto application_src = spatial_upscaler ? squasher.get_unorm_views() : src;
		auto application_rect = src_rect;
		if (spatial_upscaler)
		{
			auto upscaled = spatial_upscaler->upscale(
			        vk.device, cmd, application_src, application_rect, false, 1);
			application_src = upscaled.views;
			application_rect = upscaled.rects;
		}
		view_info.foveation = foveation.foveate(
		        vk.device,
		        cmd,
		        images[i].view_y,
		        images[i].view_cbcr,
		        flip_y,
		        application_src,
		        application_rect,
		        src_fov,
		        view_info.alpha);
	}
	else
	{
		const bool selected_source_is_linear = candidate_dispatched && frame_generation_probe.handoff_enabled;
		auto selected_src = selected_source_is_linear
		                            ? foveation_src
		                            : (spatial_upscaler ? squasher.get_unorm_views() : foveation_src);
		auto selected_rect = src_rect;
		if (spatial_upscaler)
		{
			auto upscaled = spatial_upscaler->upscale(
			        vk.device, cmd, selected_src, selected_rect, selected_source_is_linear, 0);
			selected_src = upscaled.views;
			selected_rect = upscaled.rects;
		}
		view_info.foveation = foveation.foveate(
		        vk.device,
		        cmd,
		        images[i].view_y,
		        images[i].view_cbcr,
		        flip_y,
		        selected_src,
		        selected_rect,
		        src_fov,
		        view_info.alpha);
	}
	// Keep the previous frame available to the future synthesis backend until
	// all consumers of the current composed frame have finished reading it.
	capture_frame_generation_history(cmd);
	previous_view_poses = application_poses;
	previous_view_fovs = application_fovs;
	previous_view_poses_valid = true;
	previous_display_time_ns = current_display_time_ns;

	auto add_encoder_barriers = [&](int image_index, const auto & image_info) {
		for (auto & encoder: encoders)
		{
			if (encoder->stream_idx == 2 and not image_info.alpha)
				continue;
			else if (encoder->need_transfer or encoder->target_queue == vk.queue.family_index)
			{
				image_barriers.push_back(
				        vk::ImageMemoryBarrier2{
				                .srcStageMask = vk::PipelineStageFlagBits2KHR::eComputeShader,
				                .srcAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
				                .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
				                .dstAccessMask = vk::AccessFlagBits2::eMemoryRead,
				                // For a queue-family ownership transfer in EXCLUSIVE
				                // sharing mode, the release barrier's old/new layout
				                // must match the encoder-side acquire. newLayout is
				                // the encoder's target_layout; per the VkImageMemoryBarrier2
				                // spec the layout transition is executed exactly once
				                // between the queues, so this single QFOT barrier covers
				                // both the queue-family transfer and the layout
				                // transition the encoder needs.
				                .oldLayout = vk::ImageLayout::eGeneral,
				                .newLayout = encoder->target_layout,
				                .srcQueueFamilyIndex = vk.queue.family_index,
				                .dstQueueFamilyIndex = encoder->target_queue,
				                .image = images[image_index].image,
				                .subresourceRange = {.aspectMask = vk::ImageAspectFlagBits::eColor,
				                                     .baseMipLevel = 0,
				                                     .levelCount = 1,
				                                     .baseArrayLayer = encoder->stream_idx,
				                                     .layerCount = 1},
				        });
			}
		}
	};
	add_encoder_barriers(i, view_info);
	if (generated_view_info)
		add_encoder_barriers(generated_i, *generated_view_info);

	cmd.pipelineBarrier2({
	        .imageMemoryBarrierCount = uint32_t(image_barriers.size()),
	        .pImageMemoryBarriers = image_barriers.data(),
	});
	cmd.writeTimestamp(vk::PipelineStageFlagBits::eBottomOfPipe, *query_pool, 2);

	cmd.end();

	const vk::SemaphoreSubmitInfo sem_info{
	        .semaphore = *sem,
	        .value = ++sem_value,
	        .stageMask = vk::PipelineStageFlagBits2::eComputeShader,
	};

	{
		vk::CommandBufferSubmitInfo cmd_info{
		        .commandBuffer = cmd,
		};
		std::unique_lock lock{vk.queue.mutex};
		vk.queue.queue.submit2(vk::SubmitInfo2{
		        .commandBufferInfoCount = 1,
		        .pCommandBufferInfos = &cmd_info,
		        .signalSemaphoreInfoCount = 1,
		        .pSignalSemaphoreInfos = &sem_info,
		});
	}

	pacer.mark_timing_point(COMP_TARGET_TIMING_POINT_SUBMIT_END, frame.rendering.id, os_monotonic_get_ns());
	const auto info = pacer.present_to_info(frame.rendering.desired_present_time_ns);
	// Keep the normal single-output path valid: generated_i is -1 unless
	// double-rate transport acquired a second image.  Do not put that
	// sentinel in the slot consumed by the single-output loop.
	std::array<int, 2> output_indices{i, generated_i};
	if (generated_i >= 0)
		output_indices = {generated_i, i};
	const size_t output_count = generated_i >= 0 ? 2 : 1;
	for (size_t output = 0; output < output_count; ++output)
	{
		const auto image_index = output_indices[output];
		auto & output_image = images[image_index];
		for (auto & encoder: encoders)
		{
			if (encoder->stream_idx == 2 and not output_image.view_info.alpha)
				continue;
			encoder->present_image(
			        output_image.image,
			        sem_info,
			        frame_generation_probe.double_rate_enabled ? output_image.frame_index : info.frame_id);
		}
	}
	{
		std::lock_guard lock(encode_mutex);
		for (size_t output = 0; output < output_count; ++output)
			encode_requests.push_back(output_indices[output]);
	}
	encode_cv.notify_all();
	if (generated_i >= 0 && frame_generation_probe.double_rate_pairs++ < 3)
	{
		U_LOG_IFL_I(log_level,
		            "FSR double-rate pair submitted: candidate_frame=%" PRIu64 " application_frame=%" PRIu64
		            " candidate_display=%" PRIi64 " application_display=%" PRIi64,
		            images[generated_i].frame_index,
		            images[i].frame_index,
		            generated_view_info->display_time,
		            view_info.display_time);
	}

#ifdef XRT_FEATURE_RENDERDOC
	if (auto r = renderdoc())
		r->EndFrameCapture(NULL, NULL);
#endif

	comp_frame_clear_locked(&frame.rendering);

	if (vk.device.waitSemaphores(vk::SemaphoreWaitInfo{
	                                     .semaphoreCount = 1,
	                                     .pSemaphores = &*sem,
	                                     .pValues = &sem_info.value,
	                             },
	                             U_TIME_1S_IN_NS) == vk::Result::eTimeout)
	{
		U_LOG_IFL_W(log_level, "compositor timeout");
	}
	else
	{
		auto [res, ts] = query_pool.getResults<uint64_t>(
		        0,
		        3,
		        3 * sizeof(uint64_t),
		        sizeof(uint64_t),
		        vk::QueryResultFlagBits::e64 | vk::QueryResultFlagBits::eWait);

		if (res == vk::Result::eSuccess)
		{
			static const auto period = vk.physical_device.getProperties().limits.timestampPeriod;
			squasher_times.add((ts[1] - ts[0]) * period / 1e3);
			foveation_times.add((ts[2] - ts[1]) * period / 1e3);
		}
	}

	// Now is a good point to garbage collect.
	comp_swapchain_shared_garbage_collect(&cscs);

	return XRT_SUCCESS;
}

xrt_result_t compositor::get_display_refresh_rate(float * hz)
{
	auto settings = session.get_settings();
	// there should not be rounding errors, hz must be one of the available refresh rates
	*hz = frame_rate * settings->fps_divider;
	assert(std::ranges::contains(session.get_info().available_refresh_rates, *hz));
	return XRT_SUCCESS;
}

xrt_result_t compositor::request_display_refresh_rate(float hz)
{
	requested_refresh_rate = hz;
	U_LOG_I("request refresh rate: %fHz", hz);
	if (hz > 0)
	{
		try
		{
			session.send_control(to_headset::refresh_rate_change{.hz = hz});
		}
		catch (std::exception & e)
		{
			U_LOG_W("refresh rate change failed: %s", e.what());
		}
	}
	return XRT_SUCCESS;
}

xrt_result_t compositor::get_view_config(
        xrt_compositor_native * self,
        xrt_view_type view_type,
        xrt_view_config * out_view_config)
{
	const auto & session = reinterpret_cast<compositor *>(self)->session;
	const auto extent = render_extent(session.get_info());
	switch (view_type)
	{
		case XRT_VIEW_TYPE_MONO:
			return XRT_ERROR_UNSUPPORTED_VIEW_TYPE;
		case XRT_VIEW_TYPE_STEREO:
			*out_view_config = {
			        .view_type = XRT_VIEW_TYPE_STEREO,
			        .view_count = 2,
			        .views = {}};
			for (auto & view: std::span(out_view_config->views, out_view_config->view_count))
			{
				view = {
				        .recommended = {
				                .width_pixels = extent.width,
				                .height_pixels = extent.height,
				                .sample_count = 1,
				        },
				        .max = {
				                .width_pixels = extent.width * 2u,
				                .height_pixels = extent.height * 2u,
				                .sample_count = 1,
				        },
				};
			}
			return XRT_SUCCESS;
	}
	return XRT_ERROR_UNSUPPORTED_VIEW_TYPE;
}

int compositor::acquire_image()
{
	for (auto [i, image]: std::ranges::enumerate_view(images))
	{
		if (not image.busy.exchange(true))
			return i;
	}
	return -1;
}

void compositor::encoder_work(std::stop_token tok)
{
	while (not tok.stop_requested())
	{
		size_t req;
		{
			std::unique_lock lock(encode_mutex);
			encode_cv.wait(lock, [&] { return tok.stop_requested() || not encode_requests.empty(); });
			if (tok.stop_requested())
				break;
			req = encode_requests.front();
			encode_requests.pop_front();
		}

		if (req >= images.size())
		{
			U_LOG_W("invalid compositor encode request %zu", req);
			continue;
		}
		auto & image = images[req];
		if (paused.load())
		{
			for (auto & encoder: encoders)
			{
				if (encoder->stream_idx < 2 || image.view_info.alpha)
					encoder->discard_frame();
			}
			image.busy = false;
			continue;
		}

		wivrn::trace::scope trace_iter(wivrn::trace::cpu_track::compositor, 0, image.frame_index, "encoder_work iter");

		try
		{
			for (auto & encoder: encoders)
			{
				if (encoder->stream_idx < 2 or image.view_info.alpha)
					encoder->encode(session, image.view_info, image.frame_index);
			}
		}
		catch (std::exception & e)
		{
			U_LOG_W("encode error: %s", e.what());
		}
		image.busy = false;
	}
}

void compositor::send_video_stream_description()

{
	to_headset::video_stream_description desc{
	        .width = uint16_t(images[0].image.info().extent.width),
	        .height = uint16_t(images[0].image.info().extent.height),
	        // Double-rate generation halves the application cadence and fills
	        // the interval candidate, so the transport still matches the
	        // headset refresh rate instead of advertising an unusable 180 Hz.
	        .frame_rate = frame_rate.load(),
	};
	get_display_refresh_rate(&desc.refresh_rate);
	static_assert(std::tuple_size_v<decltype(settings)> == std::tuple_size_v<decltype(desc.codec)>);
	std::ranges::transform(settings, desc.codec.begin(), &encoder_settings::codec);
	U_LOG_IFL_I(log_level, "Video stream description: size=%ux%u frame_rate=%.1fHz refresh_rate=%.1fHz", desc.width, desc.height, desc.frame_rate, desc.refresh_rate);
	session.send_control(std::move(desc));
}

compositor::compositor(wivrn_session & session) :
        comp_base{
                .base = {
                        .base = {
                                .info{},
                                .begin_session = method_pointer<&compositor::begin_session>,
                                .end_session = method_pointer<&compositor::end_session>,
                                .predict_frame = method_pointer<&compositor::predict_frame>,
                                .mark_frame = method_pointer<&compositor::mark_frame>,
                                .begin_frame = method_pointer<&compositor::begin_frame>,
                                .discard_frame = method_pointer<&compositor::discard_frame>,
                                .layer_commit = method_pointer<&compositor::layer_commit>,
                                .get_display_refresh_rate = method_pointer<&compositor::get_display_refresh_rate>,
                                .request_display_refresh_rate = method_pointer<&compositor::request_display_refresh_rate>,
                                .destroy = method_pointer<&compositor::destroy>,
                        },
                },
        },
        log_level(debug_get_log_option_log()),
        session(session),
        cmd_pool(vk.device, vk::CommandPoolCreateInfo{
                                    .flags = vk::CommandPoolCreateFlagBits::eTransient,
                                    .queueFamilyIndex = vk.queue.family_index,
                            }),
        query_pool(vk.device, vk::QueryPoolCreateInfo{
                                      .queryType = vk::QueryType::eTimestamp,
                                      .queryCount = 3,
                              }),
        settings(get_encoder_settings(vk, session)),
        images{make_images(vk, cmd_pool, settings)},
        cmd{std::move(vk.device.allocateCommandBuffers({.commandPool = *cmd_pool, .commandBufferCount = 1})[0])},
        sem{make_semaphore(vk)},
        frame_rate(settings[0].fps),
        pacer(U_TIME_1S_IN_NS / frame_rate),
		squasher(vk, render_extent(session.get_info())),
		foveation(vk, images[0].image.info().extent)
{
	const auto input_extent = render_extent(session.get_info());
	const auto upscale_extent = fsr_output_extent(session.get_info());
	if (upscale_extent.width != input_extent.width || upscale_extent.height != input_extent.height)
	{
		try
		{
			spatial_upscaler.emplace(vk, input_extent, upscale_extent);
			U_LOG_IFL_I(log_level,
			            "FSR 1 spatial upscaling enabled: EASU %ux%u -> %ux%u per eye, RCAS sharpness=%.2f stops",
			            input_extent.width,
			            input_extent.height,
			            upscale_extent.width,
			            upscale_extent.height,
			            fsr_rcas_sharpness_stops());
		}
		catch (const std::exception & e)
		{
			spatial_upscaler.reset();
			U_LOG_IFL_W(log_level, "FSR 1 spatial upscaling initialization failed: %s; using native compositor path", e.what());
		}
	}
	else if (fsr_upscaling_requested())
	{
		U_LOG_IFL_W(log_level,
		            "FSR 1 spatial upscaling requested but inactive: render=%ux%u native=%ux%u (choose a lower headset render resolution; EASU supports at most 4x output area)",
		            input_extent.width,
		            input_extent.height,
		            session.get_info().native_eye_width,
		            session.get_info().native_eye_height);
	}

	const char * frame_generation_env = std::getenv("WIVRN_FSR_FRAME_GENERATION");
	const bool frame_generation_requested = debug_get_bool_option_fsr_frame_generation();
	if (frame_generation_requested && !fsr_frame_generation_enabled())
		U_LOG_IFL_W(log_level,
		            "FSR frame generation is disabled in this build; ignoring WIVRN_FSR_FRAME_GENERATION=%s",
		            frame_generation_env != nullptr ? frame_generation_env : "<unset>");
	if (fsr_frame_generation_enabled() && frame_generation_requested)
	{
		frame_generation_probe.requested = true;
		frame_generation_probe.handoff_requested = debug_get_bool_option_fsr_frame_generation_handoff();
		frame_generation_probe.double_rate_requested = debug_get_bool_option_fsr_frame_generation_double_rate();
		frame_generation_probe.color_only_requested = debug_get_bool_option_fsr_frame_generation_color_only();
		try
		{
			const auto extent = render_extent(session.get_info());
			frame_history.emplace(vk, extent);
			frame_generation.emplace(vk, extent);
			if (frame_generation_probe.color_only_requested)
			{
				try
				{
					frame_generation_flow_backend.emplace(vk, extent);
				}
				catch (const std::exception & e)
				{
					frame_generation_probe.color_only_requested = false;
					U_LOG_IFL_W(log_level, "FSR color-only motion backend initialization failed: %s; metadata path remains enabled", e.what());
				}
			}
			frame_generation_probe.history_enabled = true;
			frame_generation_probe.backend_enabled = true;
			frame_generation_probe.handoff_enabled = frame_generation_probe.handoff_requested ||
			                                         frame_generation_probe.double_rate_requested;
			frame_generation_probe.double_rate_enabled = frame_generation_probe.double_rate_requested;
			pacer.set_stream_frame_multiplier(frame_generation_probe.double_rate_enabled ? 2 : 1);
			U_LOG_IFL_I(log_level, "FSR frame-generation compositor probe enabled: persistent color history views=2 extent=%ux%u, %s", extent.width, extent.height, frame_generation_probe.double_rate_enabled ? (frame_generation_probe.color_only_requested ? "double-rate candidate handoff requested; color-only fallback enabled" : "double-rate candidate handoff requested") : (frame_generation_probe.color_only_requested ? "color-only candidate fallback requested" : (frame_generation_probe.handoff_enabled ? "candidate handoff requested" : "normal composition retained")));
			if (frame_generation_probe.color_only_requested)
				U_LOG_IFL_I(log_level,
				            "FSR color-only optical-flow search radius: %d (override with WIVRN_FSR_FRAME_GENERATION_SEARCH_RADIUS)",
				            frame_generation_search_radius());
		}
		catch (const std::exception & e)
		{
			frame_generation.reset();
			frame_history.reset();
			frame_generation_probe.backend_enabled = false;
			frame_generation_probe.handoff_enabled = false;
			frame_generation_probe.double_rate_enabled = false;
			frame_generation_probe.color_only_requested = false;
			frame_generation_flow_backend.reset();
			pacer.set_stream_frame_multiplier(1);
			U_LOG_IFL_W(log_level, "FSR frame-generation backend initialization failed: %s; falling back to normal composition", e.what());
		}
	}

	comp_base * c_base = this;
	// Ensure we can safely cast pointers
	assert(intptr_t(&base) == intptr_t(this));
	auto res = vk_init_from_given(
	        &c_base->vk,
	        (PFN_vkGetInstanceProcAddr)vk.instance.getProcAddr("vkGetInstanceProcAddr"),
	        *vk.instance,
	        *vk.physical_device,
	        *vk.device,
	        vk.queue.family_index,
	        0,
	        vk.has_device_ext(VK_KHR_EXTERNAL_FENCE_FD_EXTENSION_NAME),
	        vk.has_device_ext(VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME),
	        std::get<vk::PhysicalDeviceVulkan12Features>(vk.feat).timelineSemaphore,
	        true, // in Vulkan 1.2
	        vk.has_instance_ext(VK_EXT_DEBUG_UTILS_EXTENSION_NAME),
	        log_level);
	vk::detail::resultCheck(vk::Result(res), "vk_init_from_given");

	// vk_init_from_given can't enable calibrated timestamps; do it here.
#ifdef VK_EXT_calibrated_timestamps
	c_base->vk.has_EXT_calibrated_timestamps = vk.has_device_ext(VK_EXT_CALIBRATED_TIMESTAMPS_EXTENSION_NAME);
#endif

	// Share monado's vk_bundle so gpu_timestamp_pool reuses its calibration cache.
	wivrn::trace::set_calibration_source(&c_base->vk);

	// vk_init_from_given assumes a graphics queue was provided
	c_base->vk.graphics_queue = nullptr;

	// Monado submits to the main queue from IPC client threads under
	// main_queue->mutex: our own submissions must take the same lock.
	vk::detail::resultCheck(vk::Result(vk_init_mutex(&c_base->vk)), "vk_init_mutex");
	if (c_base->vk.main_queue)
		vk.queue.mutex.share(c_base->vk.main_queue->mutex.mutex);

	{
		comp_vulkan_formats formats{};
		comp_vulkan_formats_check(&c_base->vk, &formats);
		comp_vulkan_formats_copy_to_info(&formats, &base.base.info);
		comp_vulkan_formats_log(log_level, &formats);
	}

	comp_base_init(this);

	// Tie the lifetimes of swapchains to Vulkan.
	xrt_result_t xret = comp_swapchain_shared_init(&cscs, &c_base->vk);
	if (xret != XRT_SUCCESS)
		throw std::runtime_error("comp_swapchain_shared_init failed");

	print_encoders(settings);
	for (auto [i, settings]: std::ranges::enumerate_view(settings))
		encoders[i] = video_encoder::create(vk, settings, i);
	if (frame_generation_probe.double_rate_enabled)
	{
		pacer.set_frame_duration(uint64_t(2.0 * double(U_TIME_1S_IN_NS) / frame_rate.load()));
		for (auto & encoder: encoders)
			encoder->set_framerate(frame_rate);
		U_LOG_IFL_I(log_level,
		            "FSR frame-generation double-rate transport enabled: application rate %.0fHz, stream rate %.0fHz",
		            frame_rate.load() * 0.5f,
		            frame_rate.load());
	}
	send_video_stream_description();

	u_var_add_root(this, "Compositor", false);
	u_var_add_f32_timing(this, &squasher_times.var, "layers processing");
	u_var_add_f32_timing(this, &foveation_times.var, "foveation");

	// Start the thread after everything is initialized
	encoder_thread = std::jthread{[&](std::stop_token t) { encoder_work(t); }};
}

compositor::~compositor()
{
	u_var_remove_root(this);
	encoder_thread.request_stop();
	encode_cv.notify_all();
	if (encoder_thread.joinable())
		encoder_thread.join();
	comp_base * c_base = this;
	comp_swapchain_shared_garbage_collect(&cscs);
	comp_swapchain_shared_destroy(&cscs, &c_base->vk);
	comp_base_fini(this);
}

xrt_system_compositor_info compositor::sys_info() const
{
	const auto & info = session.get_info();
	auto [prop, dev_id] = vk.physical_device.getProperties2<vk::PhysicalDeviceProperties2, vk::PhysicalDeviceIDProperties>();
	xrt_system_compositor_info res{
	        .view_type_count = 1,
	        .view_types = {
	                XRT_VIEW_TYPE_STEREO,
	        },
	        .max_layers = squasher.max_layers(prop.properties),
	        .supported_blend_modes = {
	                XRT_BLEND_MODE_OPAQUE,
	                XRT_BLEND_MODE_ALPHA_BLEND,
	        },
	        .supported_blend_mode_count = uint8_t(1 + info.passthrough),
	        .refresh_rate_count = std::min<uint32_t>(info.available_refresh_rates.size(), std::size(res.refresh_rates_hz)),
	        .supports_fov_mutable = true,
	};

	std::ranges::copy(dev_id.deviceUUID, res.compositor_vk_deviceUUID.data);
	std::ranges::copy(dev_id.deviceUUID, res.client_vk_deviceUUID.data);
	std::ranges::copy(std::span(info.available_refresh_rates).subspan(0, res.refresh_rate_count), res.refresh_rates_hz);
	return res;
}

void compositor::set_bitrate(uint32_t bitrate)
{
	for (auto & encoder: encoders)
		encoder->set_bitrate(bitrate);
}

void compositor::set_framerate(float hz)
{
	if (frame_rate.exchange(hz) == hz)
		return;
	U_LOG_IFL_D(log_level, "Framerate change from %.0f to %.0f", frame_rate.load(), hz);
	pacer.set_frame_duration(uint64_t(double(U_TIME_1S_IN_NS) / hz *
	                                  (frame_generation_probe.double_rate_enabled ? 2.0 : 1.0)));
	for (auto & encoder: encoders)
		encoder->set_framerate(hz);
}

void compositor::update_tracking(const from_headset::tracking & tracking)
{
	foveation.update_tracking(tracking);
}

void compositor::update_foveation_center_override(const from_headset::override_foveation_center & center)
{
	foveation.update_foveation_center_override(center);
}

void compositor::resume()
{
	std::lock_guard frame_lock(frame_mutex);
	if (!paused.load())
	{
		// The first session start also needs descriptors and an IDR.
		for (auto & encoder: encoders)
			encoder->reset();
		send_video_stream_description();
		return;
	}

	for (auto & encoder: encoders)
		encoder->reset();
	pacer.reset();
	frame_generation_probe.frames = 0;
	frame_generation_probe.reported = false;
	frame_generation_probe.last_inputs_ready = false;
	frame_generation_probe.last_ready_views = 0;
	if (frame_history)
		frame_history->valid = false;
	if (frame_generation)
		frame_generation->output_valid = false;
	if (frame_generation_flow_backend)
		frame_generation_flow_backend->output_valid = false;
	previous_display_time_ns = 0;
	previous_view_poses_valid = false;
	send_video_stream_description();
	paused = false;
}

void compositor::pause()
{
	if (paused.exchange(true))
		return;
	std::lock_guard frame_lock(frame_mutex);

	std::deque<size_t> pending_requests;
	{
		std::lock_guard encode_lock(encode_mutex);
		pending_requests.swap(encode_requests);
	}
	for (const auto request: pending_requests)
	{
		if (request >= images.size())
			continue;
		for (auto & encoder: encoders)
		{
			if (encoder->stream_idx < 2 || images[request].view_info.alpha)
				encoder->discard_frame();
		}
		images[request].busy = false;
	}
	encode_cv.notify_all();
	comp_frame_clear_locked(&frame.waited);
	comp_frame_clear_locked(&frame.rendering);
	pacer.reset();
	frame_generation_probe.frames = 0;
	frame_generation_probe.reported = false;
	frame_generation_probe.last_inputs_ready = false;
	frame_generation_probe.last_ready_views = 0;
	if (frame_history)
		frame_history->valid = false;
	if (frame_generation)
		frame_generation->output_valid = false;
	if (frame_generation_flow_backend)
		frame_generation_flow_backend->output_valid = false;
	previous_display_time_ns = 0;
	previous_view_poses_valid = false;
}

void compositor::on_feedback(const from_headset::feedback & feedback, const clock_offset & o)
{
	uint8_t stream = feedback.stream_index;
	if (stream >= encoders.size())
		return;
	encoders[stream]->on_feedback(feedback);
	if (not o)
		return;
	pacer.on_feedback(feedback, o);
}

} // namespace wivrn
