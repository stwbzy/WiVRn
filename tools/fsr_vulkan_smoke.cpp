/*
 * WiVRn VR streaming
 * Copyright (C) 2026 WiVRn-fsr contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <vulkan/vulkan.h>

#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
void check(VkResult result, const char * operation)
{
	if (result != VK_SUCCESS)
		throw std::runtime_error(std::string(operation) + " failed: " + std::to_string(result));
}

std::vector<uint32_t> read_spirv(const char * path)
{
	std::ifstream file(path, std::ios::binary | std::ios::ate);
	if (!file)
		throw std::runtime_error(std::string("cannot open ") + path);
	const auto size = file.tellg();
	if (size <= 0 || size % 4 != 0)
		throw std::runtime_error(std::string("invalid SPIR-V size: ") + path);
	std::vector<uint32_t> words(size_t(size) / sizeof(uint32_t));
	file.seekg(0);
	file.read(reinterpret_cast<char *>(words.data()), size);
	return words;
}

struct image
{
	VkImage image = VK_NULL_HANDLE;
	VkDeviceMemory memory = VK_NULL_HANDLE;
	VkImageView view = VK_NULL_HANDLE;
};

struct buffer
{
	VkBuffer buffer = VK_NULL_HANDLE;
	VkDeviceMemory memory = VK_NULL_HANDLE;
};

uint32_t memory_type(VkPhysicalDevice physical_device, uint32_t bits, VkMemoryPropertyFlags flags)
{
	VkPhysicalDeviceMemoryProperties properties{};
	vkGetPhysicalDeviceMemoryProperties(physical_device, &properties);
	for (uint32_t i = 0; i < properties.memoryTypeCount; ++i)
		if ((bits & (1u << i)) && (properties.memoryTypes[i].propertyFlags & flags) == flags)
			return i;
	throw std::runtime_error("no compatible Vulkan memory type");
}

image make_image(VkPhysicalDevice physical_device,
	             VkDevice device,
	             VkFormat format,
	             uint32_t width,
	             uint32_t height,
	             VkImageUsageFlags usage)
{
	image result;
	const VkImageCreateInfo create{
	        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
	        .imageType = VK_IMAGE_TYPE_2D,
	        .format = format,
	        .extent = {width, height, 1},
	        .mipLevels = 1,
	        .arrayLayers = 1,
	        .samples = VK_SAMPLE_COUNT_1_BIT,
	        .tiling = VK_IMAGE_TILING_OPTIMAL,
	        .usage = usage,
	        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
	        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
	};
	check(vkCreateImage(device, &create, nullptr, &result.image), "vkCreateImage");
	VkMemoryRequirements requirements{};
	vkGetImageMemoryRequirements(device, result.image, &requirements);
	const VkMemoryAllocateInfo allocate{
	        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
	        .allocationSize = requirements.size,
	        .memoryTypeIndex = memory_type(physical_device, requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT),
	};
	check(vkAllocateMemory(device, &allocate, nullptr, &result.memory), "vkAllocateMemory(image)");
	check(vkBindImageMemory(device, result.image, result.memory, 0), "vkBindImageMemory");
	const VkImageViewCreateInfo view{
	        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
	        .image = result.image,
	        .viewType = VK_IMAGE_VIEW_TYPE_2D,
	        .format = format,
	        .subresourceRange = {
	                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
	                .baseMipLevel = 0,
	                .levelCount = 1,
	                .baseArrayLayer = 0,
	                .layerCount = 1,
	        },
	};
	check(vkCreateImageView(device, &view, nullptr, &result.view), "vkCreateImageView");
	return result;
}

buffer make_buffer(VkPhysicalDevice physical_device, VkDevice device, VkDeviceSize size, VkBufferUsageFlags usage)
{
	buffer result;
	const VkBufferCreateInfo create{
	        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
	        .size = size,
	        .usage = usage,
	        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
	};
	check(vkCreateBuffer(device, &create, nullptr, &result.buffer), "vkCreateBuffer");
	VkMemoryRequirements requirements{};
	vkGetBufferMemoryRequirements(device, result.buffer, &requirements);
	const VkMemoryAllocateInfo allocate{
	        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
	        .allocationSize = requirements.size,
	        .memoryTypeIndex = memory_type(
	                physical_device,
	                requirements.memoryTypeBits,
	                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT),
	};
	check(vkAllocateMemory(device, &allocate, nullptr, &result.memory), "vkAllocateMemory(buffer)");
	check(vkBindBufferMemory(device, result.buffer, result.memory, 0), "vkBindBufferMemory");
	return result;
}

VkShaderModule make_shader(VkDevice device, const std::vector<uint32_t> & words)
{
	const VkShaderModuleCreateInfo create{
	        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
	        .codeSize = words.size() * sizeof(uint32_t),
	        .pCode = words.data(),
	};
	VkShaderModule result;
	check(vkCreateShaderModule(device, &create, nullptr, &result), "vkCreateShaderModule");
	return result;
}

VkPipeline make_pipeline(VkDevice device, VkPipelineLayout layout, VkShaderModule shader)
{
	const VkComputePipelineCreateInfo create{
	        .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
	        .stage = {
	                .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
	                .stage = VK_SHADER_STAGE_COMPUTE_BIT,
	                .module = shader,
	                .pName = "main",
	        },
	        .layout = layout,
	};
	VkPipeline result;
	check(vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &create, nullptr, &result), "vkCreateComputePipelines");
	return result;
}

std::atomic_uint validation_errors = 0;

VKAPI_ATTR VkBool32 VKAPI_CALL validation_callback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
	                                                VkDebugUtilsMessageTypeFlagsEXT,
	                                                const VkDebugUtilsMessengerCallbackDataEXT * data,
	                                                void *)
{
	if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)
		++validation_errors;
	std::cerr << "Vulkan validation: " << data->pMessage << '\n';
	return VK_FALSE;
}

struct alignas(16) easu_constants
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
static_assert(sizeof(easu_constants) == 96);

struct alignas(16) rcas_constants
{
	std::array<int32_t, 2> output_offset;
	std::array<uint32_t, 2> output_size;
	float sharpness;
	std::array<float, 3> padding{};
};
static_assert(sizeof(rcas_constants) == 32);
} // namespace

int main(int argc, char ** argv)
try
{
	if (argc != 3)
	{
		std::cerr << "usage: wivrn-fsr-vulkan-smoke EASU.spv RCAS.spv\n";
		return 2;
	}
	constexpr uint32_t input_width = 16;
	constexpr uint32_t input_height = 16;
	constexpr uint32_t output_width = 32;
	constexpr uint32_t output_height = 32;
	constexpr VkDeviceSize input_eye_bytes = input_width * input_height * 4;
	constexpr VkDeviceSize output_eye_bytes = output_width * output_height * 4 * sizeof(uint16_t);

	const bool validation = std::getenv("WIVRN_VULKAN_VALIDATION") != nullptr;
	const char * validation_layer = "VK_LAYER_KHRONOS_validation";
	const char * debug_extension = VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
	const VkApplicationInfo application{
	        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
	        .pApplicationName = "WiVRn FSR spatial smoke",
	        .applicationVersion = 1,
	        .pEngineName = "none",
	        .engineVersion = 1,
	        .apiVersion = VK_API_VERSION_1_3,
	};
	const VkInstanceCreateInfo instance_create{
	        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
	        .pApplicationInfo = &application,
	        .enabledLayerCount = validation ? 1u : 0u,
	        .ppEnabledLayerNames = validation ? &validation_layer : nullptr,
	        .enabledExtensionCount = validation ? 1u : 0u,
	        .ppEnabledExtensionNames = validation ? &debug_extension : nullptr,
	};
	VkInstance instance;
	check(vkCreateInstance(&instance_create, nullptr, &instance), "vkCreateInstance");
	VkDebugUtilsMessengerEXT messenger = VK_NULL_HANDLE;
	if (validation)
	{
		const VkDebugUtilsMessengerCreateInfoEXT create{
		        .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
		        .messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
		                           VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
		        .messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
		                       VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
		                       VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT,
		        .pfnUserCallback = validation_callback,
		};
		auto create_messenger = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
		        vkGetInstanceProcAddr(instance, "vkCreateDebugUtilsMessengerEXT"));
		check(create_messenger(instance, &create, nullptr, &messenger), "vkCreateDebugUtilsMessengerEXT");
	}

	uint32_t physical_count = 0;
	check(vkEnumeratePhysicalDevices(instance, &physical_count, nullptr), "vkEnumeratePhysicalDevices(count)");
	if (physical_count == 0)
		throw std::runtime_error("no Vulkan physical device");
	std::vector<VkPhysicalDevice> physical_devices(physical_count);
	check(vkEnumeratePhysicalDevices(instance, &physical_count, physical_devices.data()), "vkEnumeratePhysicalDevices");
	VkPhysicalDevice physical_device = physical_devices.front();
	for (auto candidate: physical_devices)
	{
		VkPhysicalDeviceProperties properties{};
		vkGetPhysicalDeviceProperties(candidate, &properties);
		if (properties.vendorID == 0x8086 && properties.deviceID == 0x56a1)
			physical_device = candidate;
	}
	VkPhysicalDeviceProperties physical_properties{};
	vkGetPhysicalDeviceProperties(physical_device, &physical_properties);
	std::cout << "FSR Vulkan smoke device: " << physical_properties.deviceName << '\n';

	uint32_t family_count = 0;
	vkGetPhysicalDeviceQueueFamilyProperties(physical_device, &family_count, nullptr);
	std::vector<VkQueueFamilyProperties> families(family_count);
	vkGetPhysicalDeviceQueueFamilyProperties(physical_device, &family_count, families.data());
	uint32_t family = UINT32_MAX;
	for (uint32_t i = 0; i < family_count; ++i)
		if (families[i].queueFlags & VK_QUEUE_COMPUTE_BIT)
		{
			family = i;
			break;
		}
	if (family == UINT32_MAX)
		throw std::runtime_error("no Vulkan compute queue");
	const float priority = 1.0f;
	const VkDeviceQueueCreateInfo queue_create{
	        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
	        .queueFamilyIndex = family,
	        .queueCount = 1,
	        .pQueuePriorities = &priority,
	};
	const VkDeviceCreateInfo device_create{
	        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
	        .queueCreateInfoCount = 1,
	        .pQueueCreateInfos = &queue_create,
	};
	VkDevice device;
	check(vkCreateDevice(physical_device, &device_create, nullptr, &device), "vkCreateDevice");
	VkQueue queue;
	vkGetDeviceQueue(device, family, 0, &queue);

	const auto sampled_usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
	const auto compute_usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
	std::array<image, 2> source{
	        make_image(physical_device, device, VK_FORMAT_R8G8B8A8_UNORM, input_width, input_height, sampled_usage),
	        make_image(physical_device, device, VK_FORMAT_R8G8B8A8_UNORM, input_width, input_height, sampled_usage),
	};
	std::array<image, 2> easu{
	        make_image(physical_device, device, VK_FORMAT_R16G16B16A16_SFLOAT, output_width, output_height, compute_usage),
	        make_image(physical_device, device, VK_FORMAT_R16G16B16A16_SFLOAT, output_width, output_height, compute_usage),
	};
	std::array<image, 2> output{
	        make_image(physical_device, device, VK_FORMAT_R16G16B16A16_SFLOAT, output_width, output_height, compute_usage),
	        make_image(physical_device, device, VK_FORMAT_R16G16B16A16_SFLOAT, output_width, output_height, compute_usage),
	};
	buffer upload = make_buffer(physical_device, device, input_eye_bytes * 2, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
	buffer readback = make_buffer(physical_device, device, output_eye_bytes * 2, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
	void * upload_data;
	check(vkMapMemory(device, upload.memory, 0, VK_WHOLE_SIZE, 0, &upload_data), "vkMapMemory(upload)");
	auto * pixels = static_cast<uint8_t *>(upload_data);
	for (uint32_t eye = 0; eye < 2; ++eye)
		for (uint32_t y = 0; y < input_height; ++y)
			for (uint32_t x = 0; x < input_width; ++x)
			{
				const bool checker = ((x / 4) + (y / 4)) % 2;
				auto * pixel = pixels + eye * input_eye_bytes + (y * input_width + x) * 4;
				const std::array<uint8_t, 4> left = checker ? std::array<uint8_t, 4>{255, 32, 32, 255}
				                                                 : std::array<uint8_t, 4>{32, 255, 32, 255};
				const std::array<uint8_t, 4> right = checker ? std::array<uint8_t, 4>{32, 32, 255, 255}
				                                                  : std::array<uint8_t, 4>{255, 255, 32, 255};
				std::memcpy(pixel, eye == 0 ? left.data() : right.data(), 4);
			}
	vkUnmapMemory(device, upload.memory);

	const std::array<VkDescriptorSetLayoutBinding, 2> bindings{
	        VkDescriptorSetLayoutBinding{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
	        VkDescriptorSetLayoutBinding{1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
	};
	const VkDescriptorSetLayoutCreateInfo descriptor_layout_create{
	        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
	        .bindingCount = uint32_t(bindings.size()),
	        .pBindings = bindings.data(),
	};
	VkDescriptorSetLayout descriptor_layout;
	check(vkCreateDescriptorSetLayout(device, &descriptor_layout_create, nullptr, &descriptor_layout), "vkCreateDescriptorSetLayout");
	const VkPushConstantRange easu_range{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(easu_constants)};
	const VkPushConstantRange rcas_range{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(rcas_constants)};
	auto make_layout = [&](const VkPushConstantRange & range) {
		const VkPipelineLayoutCreateInfo create{
		        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
		        .setLayoutCount = 1,
		        .pSetLayouts = &descriptor_layout,
		        .pushConstantRangeCount = 1,
		        .pPushConstantRanges = &range,
		};
		VkPipelineLayout layout;
		check(vkCreatePipelineLayout(device, &create, nullptr, &layout), "vkCreatePipelineLayout");
		return layout;
	};
	const VkPipelineLayout easu_layout = make_layout(easu_range);
	const VkPipelineLayout rcas_layout = make_layout(rcas_range);
	const VkShaderModule easu_shader = make_shader(device, read_spirv(argv[1]));
	const VkShaderModule rcas_shader = make_shader(device, read_spirv(argv[2]));
	const VkPipeline easu_pipeline = make_pipeline(device, easu_layout, easu_shader);
	const VkPipeline rcas_pipeline = make_pipeline(device, rcas_layout, rcas_shader);
	const std::array<VkDescriptorPoolSize, 2> pool_sizes{
	        VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 4},
	        VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 4},
	};
	const VkDescriptorPoolCreateInfo pool_create{
	        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
	        .maxSets = 4,
	        .poolSizeCount = uint32_t(pool_sizes.size()),
	        .pPoolSizes = pool_sizes.data(),
	};
	VkDescriptorPool descriptor_pool;
	check(vkCreateDescriptorPool(device, &pool_create, nullptr, &descriptor_pool), "vkCreateDescriptorPool");
	std::array<VkDescriptorSetLayout, 4> descriptor_layouts;
	descriptor_layouts.fill(descriptor_layout);
	const VkDescriptorSetAllocateInfo descriptor_allocate{
	        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
	        .descriptorPool = descriptor_pool,
	        .descriptorSetCount = uint32_t(descriptor_layouts.size()),
	        .pSetLayouts = descriptor_layouts.data(),
	};
	std::array<VkDescriptorSet, 4> descriptor_sets;
	check(vkAllocateDescriptorSets(device, &descriptor_allocate, descriptor_sets.data()), "vkAllocateDescriptorSets");
	const VkSamplerCreateInfo sampler_create{
	        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
	        .magFilter = VK_FILTER_LINEAR,
	        .minFilter = VK_FILTER_LINEAR,
	        .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
	        .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
	        .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
	        .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
	        .maxLod = 0.0f,
	};
	VkSampler sampler;
	check(vkCreateSampler(device, &sampler_create, nullptr, &sampler), "vkCreateSampler");
	for (uint32_t eye = 0; eye < 2; ++eye)
	{
		const std::array<VkDescriptorImageInfo, 4> infos{
		        VkDescriptorImageInfo{sampler, source[eye].view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
		        VkDescriptorImageInfo{VK_NULL_HANDLE, easu[eye].view, VK_IMAGE_LAYOUT_GENERAL},
		        VkDescriptorImageInfo{sampler, easu[eye].view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
		        VkDescriptorImageInfo{VK_NULL_HANDLE, output[eye].view, VK_IMAGE_LAYOUT_GENERAL},
		};
		const std::array<VkWriteDescriptorSet, 4> writes{
		        VkWriteDescriptorSet{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, descriptor_sets[eye], 0, 0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &infos[0], nullptr, nullptr},
		        VkWriteDescriptorSet{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, descriptor_sets[eye], 1, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &infos[1], nullptr, nullptr},
		        VkWriteDescriptorSet{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, descriptor_sets[2 + eye], 0, 0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &infos[2], nullptr, nullptr},
		        VkWriteDescriptorSet{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, descriptor_sets[2 + eye], 1, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &infos[3], nullptr, nullptr},
		};
		vkUpdateDescriptorSets(device, uint32_t(writes.size()), writes.data(), 0, nullptr);
	}

	const VkCommandPoolCreateInfo command_pool_create{
	        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
	        .flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT,
	        .queueFamilyIndex = family,
	};
	VkCommandPool command_pool;
	check(vkCreateCommandPool(device, &command_pool_create, nullptr, &command_pool), "vkCreateCommandPool");
	const VkCommandBufferAllocateInfo command_allocate{
	        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
	        .commandPool = command_pool,
	        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
	        .commandBufferCount = 1,
	};
	VkCommandBuffer command;
	check(vkAllocateCommandBuffers(device, &command_allocate, &command), "vkAllocateCommandBuffers");
	const VkCommandBufferBeginInfo begin{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
	check(vkBeginCommandBuffer(command, &begin), "vkBeginCommandBuffer");

	auto image_barrier = [](VkImage target, VkAccessFlags src_access, VkAccessFlags dst_access, VkImageLayout old_layout, VkImageLayout new_layout) {
		return VkImageMemoryBarrier{
		        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
		        .srcAccessMask = src_access,
		        .dstAccessMask = dst_access,
		        .oldLayout = old_layout,
		        .newLayout = new_layout,
		        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		        .image = target,
		        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
		};
	};
	std::array<VkImageMemoryBarrier, 6> initial_barriers;
	for (uint32_t eye = 0; eye < 2; ++eye)
	{
		initial_barriers[eye] = image_barrier(source[eye].image, 0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
		initial_barriers[2 + eye] = image_barrier(easu[eye].image, 0, VK_ACCESS_SHADER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
		initial_barriers[4 + eye] = image_barrier(output[eye].image, 0, VK_ACCESS_SHADER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
	}
	vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr, uint32_t(initial_barriers.size()), initial_barriers.data());
	for (uint32_t eye = 0; eye < 2; ++eye)
	{
		const VkBufferImageCopy copy{
		        .bufferOffset = eye * input_eye_bytes,
		        .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
		        .imageExtent = {input_width, input_height, 1},
		};
		vkCmdCopyBufferToImage(command, upload.buffer, source[eye].image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
	}
	std::array<VkImageMemoryBarrier, 2> source_barriers;
	for (uint32_t eye = 0; eye < 2; ++eye)
		source_barriers[eye] = image_barrier(source[eye].image, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
	vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr, uint32_t(source_barriers.size()), source_barriers.data());

	const easu_constants easu_push{
	        .con0 = {0.5f, 0.5f, -0.25f, -0.25f},
	        .con1 = {1.0f / 16.0f, 1.0f / 16.0f, 1.0f / 16.0f, -1.0f / 16.0f},
	        .con2 = {-1.0f / 16.0f, 2.0f / 16.0f, 1.0f / 16.0f, 2.0f / 16.0f},
	        .con3 = {0.0f, 4.0f / 16.0f, 0.0f, 0.0f},
	        .output_offset = {0, 0},
	        .output_size = {output_width, output_height},
	        .source_is_linear = 0,
	};
	vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, easu_pipeline);
	for (uint32_t eye = 0; eye < 2; ++eye)
	{
		vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, easu_layout, 0, 1, &descriptor_sets[eye], 0, nullptr);
		vkCmdPushConstants(command, easu_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(easu_push), &easu_push);
		vkCmdDispatch(command, output_width / 8, output_height / 8, 1);
	}
	std::array<VkImageMemoryBarrier, 2> easu_barriers;
	for (uint32_t eye = 0; eye < 2; ++eye)
		easu_barriers[eye] = image_barrier(easu[eye].image, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
	vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr, uint32_t(easu_barriers.size()), easu_barriers.data());

	const rcas_constants rcas_push{
	        .output_offset = {0, 0},
	        .output_size = {output_width, output_height},
	        .sharpness = std::exp2(-0.2f),
	};
	vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, rcas_pipeline);
	for (uint32_t eye = 0; eye < 2; ++eye)
	{
		vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, rcas_layout, 0, 1, &descriptor_sets[2 + eye], 0, nullptr);
		vkCmdPushConstants(command, rcas_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(rcas_push), &rcas_push);
		vkCmdDispatch(command, output_width / 8, output_height / 8, 1);
	}
	std::array<VkImageMemoryBarrier, 2> output_barriers;
	for (uint32_t eye = 0; eye < 2; ++eye)
		output_barriers[eye] = image_barrier(output[eye].image, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
	vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, uint32_t(output_barriers.size()), output_barriers.data());
	for (uint32_t eye = 0; eye < 2; ++eye)
	{
		const VkBufferImageCopy copy{
		        .bufferOffset = eye * output_eye_bytes,
		        .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
		        .imageExtent = {output_width, output_height, 1},
		};
		vkCmdCopyImageToBuffer(command, output[eye].image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback.buffer, 1, &copy);
	}
	const VkBufferMemoryBarrier host_barrier{
	        .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
	        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
	        .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
	        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
	        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
	        .buffer = readback.buffer,
	        .offset = 0,
	        .size = VK_WHOLE_SIZE,
	};
	vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 1, &host_barrier, 0, nullptr);
	check(vkEndCommandBuffer(command), "vkEndCommandBuffer");
	const VkSubmitInfo submit{.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &command};
	check(vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE), "vkQueueSubmit");
	check(vkQueueWaitIdle(queue), "vkQueueWaitIdle");

	void * readback_data;
	check(vkMapMemory(device, readback.memory, 0, VK_WHOLE_SIZE, 0, &readback_data), "vkMapMemory(readback)");
	const auto * halves = static_cast<const uint16_t *>(readback_data);
	bool non_black[2]{};
	for (uint32_t eye = 0; eye < 2; ++eye)
		for (size_t component = 0; component < output_eye_bytes / sizeof(uint16_t); ++component)
		{
			const uint16_t value = halves[eye * output_eye_bytes / sizeof(uint16_t) + component];
			if ((value & 0x7c00u) == 0x7c00u)
				throw std::runtime_error("FSR output contains NaN or infinity");
			if (component % 4 != 3 && (value & 0x7fffu) != 0)
				non_black[eye] = true;
		}
	if (!non_black[0] || !non_black[1])
		throw std::runtime_error("FSR output is black or an eye was not written");
	if (std::memcmp(readback_data, static_cast<const uint8_t *>(readback_data) + output_eye_bytes, output_eye_bytes) == 0)
		throw std::runtime_error("left and right FSR outputs are unexpectedly identical");
	vkUnmapMemory(device, readback.memory);
	if (validation_errors != 0)
		throw std::runtime_error("Vulkan validation reported " + std::to_string(validation_errors.load()) + " error(s)");
	std::cout << "FSR Vulkan smoke passed: finite, non-black, distinct stereo output\n";
	return 0;
}
catch (const std::exception & error)
{
	std::cerr << "FSR Vulkan smoke failed: " << error.what() << '\n';
	return 1;
}
