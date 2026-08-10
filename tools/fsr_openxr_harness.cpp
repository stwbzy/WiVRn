// Copyright 2026 WiVRn contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Small, opt-in OpenXR diagnostic harness for the WiVRn FSR work. It submits
// structural color/depth/motion-vector metadata only. It does not write image
// contents and it does not synthesize or replace frames.

#include <vulkan/vulkan.h>

#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace
{

struct failure : std::runtime_error
{
	using std::runtime_error::runtime_error;
};

void check_xr(XrResult result, std::string_view operation)
{
	if (XR_FAILED(result))
		throw failure(std::string(operation) + " failed with XrResult=" + std::to_string(result));
}

void check_vk(VkResult result, std::string_view operation)
{
	if (result != VK_SUCCESS)
		throw failure(std::string(operation) + " failed with VkResult=" + std::to_string(result));
}

template <typename T>
T get_xr_proc(XrInstance instance, const char * name)
{
	PFN_xrVoidFunction function = nullptr;
	check_xr(xrGetInstanceProcAddr(instance, name, &function), name);
	return reinterpret_cast<T>(function);
}

struct options
{
	enum class metadata
	{
		auto_select,
		ext,
		fb,
		none,
	};

	metadata mode = metadata::auto_select;
	uint32_t frames = 30;
	uint32_t timeout_ms = 5000;
};

options parse_options(int argc, char ** argv)
{
	options result;
	for (int i = 1; i < argc; ++i)
	{
		const std::string_view argument = argv[i];
		if (argument == "--frame-synthesis")
			result.mode = options::metadata::ext;
		else if (argument == "--space-warp")
			result.mode = options::metadata::fb;
		else if (argument == "--normal")
			result.mode = options::metadata::none;
		else if (argument == "--frames" && i + 1 < argc)
			result.frames = static_cast<uint32_t>(std::stoul(argv[++i]));
		else if (argument == "--timeout-ms" && i + 1 < argc)
			result.timeout_ms = static_cast<uint32_t>(std::stoul(argv[++i]));
		else if (argument == "--help")
		{
			std::cout << "Usage: wivrn-fsr-openxr-harness [--normal|--frame-synthesis|--space-warp] "
			             "[--frames N] [--timeout-ms N]\n";
			std::exit(0);
		}
		else
			throw failure("unknown argument: " + std::string(argument));
	}
	if (result.frames == 0)
		throw failure("--frames must be greater than zero");
	return result;
}

std::vector<XrExtensionProperties> enumerate_extensions()
{
	uint32_t count = 0;
	check_xr(xrEnumerateInstanceExtensionProperties(nullptr, 0, &count, nullptr),
	         "xrEnumerateInstanceExtensionProperties(count)");
	std::vector<XrExtensionProperties> result(count, XrExtensionProperties{XR_TYPE_EXTENSION_PROPERTIES});
	check_xr(xrEnumerateInstanceExtensionProperties(nullptr, count, &count, result.data()),
	         "xrEnumerateInstanceExtensionProperties");
	return result;
}

bool has_extension(const std::vector<XrExtensionProperties> & extensions, const char * name)
{
	return std::ranges::any_of(extensions, [name](const XrExtensionProperties & property) {
		return std::strcmp(property.extensionName, name) == 0;
	});
}

XrInstance create_instance(const std::vector<XrExtensionProperties> & available, bool use_metadata, bool use_ext)
{
	if (!has_extension(available, XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME))
		throw failure("active runtime does not expose XR_KHR_vulkan_enable2");

	std::vector<const char *> extensions{XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME};
	if (use_metadata)
	{
		const char * metadata_extension = use_ext ? XR_EXT_FRAME_SYNTHESIS_EXTENSION_NAME
		                                          : XR_FB_SPACE_WARP_EXTENSION_NAME;
		if (!has_extension(available, metadata_extension))
			throw failure(std::string("active runtime does not expose ") + metadata_extension);
		extensions.push_back(metadata_extension);
	}

	XrInstanceCreateInfo create_info{
	        .type = XR_TYPE_INSTANCE_CREATE_INFO,
	        .applicationInfo = {
	                .applicationName = "wivrn-fsr-openxr-harness",
	                .applicationVersion = 1,
	                .engineName = "wivrn-fsr-harness",
	                .engineVersion = 1,
	                .apiVersion = XR_API_VERSION_1_1,
	        },
	        .enabledExtensionCount = static_cast<uint32_t>(extensions.size()),
	        .enabledExtensionNames = extensions.data(),
	};

	XrInstance instance = XR_NULL_HANDLE;
	XrResult result = xrCreateInstance(&create_info, &instance);
	if (XR_FAILED(result))
	{
		create_info.applicationInfo.apiVersion = XR_API_VERSION_1_0;
		check_xr(xrCreateInstance(&create_info, &instance), "xrCreateInstance");
	}
	return instance;
}

struct vulkan_context
{
	VkInstance instance = VK_NULL_HANDLE;
	VkPhysicalDevice physical_device = VK_NULL_HANDLE;
	VkDevice device = VK_NULL_HANDLE;
	uint32_t queue_family = 0;
	VkQueue queue = VK_NULL_HANDLE;

	~vulkan_context()
	{
		if (device != VK_NULL_HANDLE)
			vkDestroyDevice(device, nullptr);
		if (instance != VK_NULL_HANDLE)
			vkDestroyInstance(instance, nullptr);
	}

	vulkan_context() = default;
	vulkan_context(const vulkan_context &) = delete;
	vulkan_context & operator=(const vulkan_context &) = delete;
	vulkan_context(vulkan_context && other) noexcept : instance(std::exchange(other.instance, VK_NULL_HANDLE)),
	                                                   physical_device(other.physical_device),
	                                                   device(std::exchange(other.device, VK_NULL_HANDLE)),
	                                                   queue_family(other.queue_family),
	                                                   queue(other.queue)
	{
		other.physical_device = VK_NULL_HANDLE;
		other.queue = VK_NULL_HANDLE;
	}
	vulkan_context & operator=(vulkan_context && other) noexcept
	{
		if (this != &other)
		{
			if (device != VK_NULL_HANDLE)
				vkDestroyDevice(device, nullptr);
			if (instance != VK_NULL_HANDLE)
				vkDestroyInstance(instance, nullptr);
			instance = std::exchange(other.instance, VK_NULL_HANDLE);
			physical_device = other.physical_device;
			device = std::exchange(other.device, VK_NULL_HANDLE);
			queue_family = other.queue_family;
			queue = other.queue;
			other.physical_device = VK_NULL_HANDLE;
			other.queue = VK_NULL_HANDLE;
		}
		return *this;
	}
};

vulkan_context create_vulkan(XrInstance instance, XrSystemId system)
{
	vulkan_context result;

	VkApplicationInfo application_info{
	        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
	        .pApplicationName = "wivrn-fsr-openxr-harness",
	        .applicationVersion = 1,
	        .pEngineName = "wivrn-fsr-harness",
	        .engineVersion = 1,
	        .apiVersion = VK_API_VERSION_1_1,
	};
	VkInstanceCreateInfo instance_info{
	        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
	        .pApplicationInfo = &application_info,
	};
	// XR_KHR_vulkan_enable2 does not provide the old extension-name query.
	// The diagnostic harness does not need application Vulkan extensions, so
	// let the runtime create the minimal instance and device directly.

	const auto create_vulkan_instance =
	        get_xr_proc<PFN_xrCreateVulkanInstanceKHR>(instance, "xrCreateVulkanInstanceKHR");
	XrVulkanInstanceCreateInfoKHR xr_instance_info{
	        .type = XR_TYPE_VULKAN_INSTANCE_CREATE_INFO_KHR,
	        .systemId = system,
	        .pfnGetInstanceProcAddr = vkGetInstanceProcAddr,
	        .vulkanCreateInfo = &instance_info,
	};
	VkResult vk_result = VK_SUCCESS;
	check_xr(create_vulkan_instance(instance, &xr_instance_info, &result.instance, &vk_result),
	         "xrCreateVulkanInstanceKHR");
	check_vk(vk_result, "xrCreateVulkanInstanceKHR/Vulkan");

	const auto get_graphics_device =
	        get_xr_proc<PFN_xrGetVulkanGraphicsDevice2KHR>(instance, "xrGetVulkanGraphicsDevice2KHR");
	XrVulkanGraphicsDeviceGetInfoKHR device_info{
	        .type = XR_TYPE_VULKAN_GRAPHICS_DEVICE_GET_INFO_KHR,
	        .systemId = system,
	        .vulkanInstance = result.instance,
	};
	check_xr(get_graphics_device(instance, &device_info, &result.physical_device),
	         "xrGetVulkanGraphicsDevice2KHR");

	uint32_t queue_count = 0;
	vkGetPhysicalDeviceQueueFamilyProperties(result.physical_device, &queue_count, nullptr);
	std::vector<VkQueueFamilyProperties> queues(queue_count);
	vkGetPhysicalDeviceQueueFamilyProperties(result.physical_device, &queue_count, queues.data());
	for (uint32_t i = 0; i < queue_count; ++i)
	{
		if ((queues[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0)
		{
			result.queue_family = i;
			break;
		}
	}
	if ((queues[result.queue_family].queueFlags & VK_QUEUE_GRAPHICS_BIT) == 0)
		throw failure("Vulkan physical device has no graphics queue");

	const float priority = 1.0f;
	VkDeviceQueueCreateInfo queue_info{
	        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
	        .queueFamilyIndex = result.queue_family,
	        .queueCount = 1,
	        .pQueuePriorities = &priority,
	};
	VkDeviceCreateInfo create_device_info{
	        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
	        .queueCreateInfoCount = 1,
	        .pQueueCreateInfos = &queue_info,
	};

	const auto create_vulkan_device =
	        get_xr_proc<PFN_xrCreateVulkanDeviceKHR>(instance, "xrCreateVulkanDeviceKHR");
	XrVulkanDeviceCreateInfoKHR xr_device_info{
	        .type = XR_TYPE_VULKAN_DEVICE_CREATE_INFO_KHR,
	        .systemId = system,
	        .pfnGetInstanceProcAddr = vkGetInstanceProcAddr,
	        .vulkanPhysicalDevice = result.physical_device,
	        .vulkanCreateInfo = &create_device_info,
	};
	vk_result = VK_SUCCESS;
	check_xr(create_vulkan_device(instance, &xr_device_info, &result.device, &vk_result),
	         "xrCreateVulkanDeviceKHR");
	check_vk(vk_result, "xrCreateVulkanDeviceKHR/Vulkan");
	vkGetDeviceQueue(result.device, result.queue_family, 0, &result.queue);
	return result;
}

struct swapchain
{
	XrSwapchain handle = XR_NULL_HANDLE;
	uint32_t width = 0;
	uint32_t height = 0;

	~swapchain()
	{
		if (handle != XR_NULL_HANDLE)
			xrDestroySwapchain(handle);
	}
	swapchain() = default;
	swapchain(const swapchain &) = delete;
	swapchain & operator=(const swapchain &) = delete;
	void reset()
	{
		if (handle != XR_NULL_HANDLE)
		{
			xrDestroySwapchain(handle);
			handle = XR_NULL_HANDLE;
		}
	}
	swapchain(swapchain && other) noexcept : handle(std::exchange(other.handle, XR_NULL_HANDLE)), width(other.width), height(other.height) {}
	swapchain & operator=(swapchain && other) noexcept
	{
		if (this != &other)
		{
			if (handle != XR_NULL_HANDLE)
				xrDestroySwapchain(handle);
			handle = std::exchange(other.handle, XR_NULL_HANDLE);
			width = other.width;
			height = other.height;
		}
		return *this;
	}
};

swapchain create_swapchain(XrSession session,
                           uint32_t width,
                           uint32_t height,
                           int64_t format,
                           XrSwapchainUsageFlags usage)
{
	swapchain result;
	XrSwapchainCreateInfo create_info{
	        .type = XR_TYPE_SWAPCHAIN_CREATE_INFO,
	        .usageFlags = usage,
	        .format = format,
	        .sampleCount = 1,
	        .width = width,
	        .height = height,
	        .faceCount = 1,
	        .arraySize = 1,
	        .mipCount = 1,
	};
	check_xr(xrCreateSwapchain(session, &create_info, &result.handle), "xrCreateSwapchain");
	result.width = width;
	result.height = height;

	uint32_t image_count = 0;
	check_xr(xrEnumerateSwapchainImages(result.handle, 0, &image_count, nullptr),
	         "xrEnumerateSwapchainImages(count)");
	std::vector<XrSwapchainImageVulkan2KHR> images(image_count, XrSwapchainImageVulkan2KHR{XR_TYPE_SWAPCHAIN_IMAGE_VULKAN2_KHR});
	check_xr(xrEnumerateSwapchainImages(result.handle,
	                                    image_count,
	                                    &image_count,
	                                    reinterpret_cast<XrSwapchainImageBaseHeader *>(images.data())),
	         "xrEnumerateSwapchainImages");
	return result;
}

void acquire_and_release(const std::vector<swapchain *> & swapchains)
{
	std::vector<uint32_t> acquired;
	acquired.reserve(swapchains.size());
	for (swapchain * chain: swapchains)
	{
		uint32_t index = 0;
		XrSwapchainImageAcquireInfo acquire_info{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
		check_xr(xrAcquireSwapchainImage(chain->handle, &acquire_info, &index), "xrAcquireSwapchainImage");
		XrSwapchainImageWaitInfo wait_info{
		        .type = XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO,
		        .timeout = XR_INFINITE_DURATION,
		};
		check_xr(xrWaitSwapchainImage(chain->handle, &wait_info), "xrWaitSwapchainImage");
		acquired.push_back(index);
	}
	for (swapchain * chain: swapchains)
	{
		XrSwapchainImageReleaseInfo release_info{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
		check_xr(xrReleaseSwapchainImage(chain->handle, &release_info), "xrReleaseSwapchainImage");
	}
}

int64_t choose_format(const std::vector<int64_t> & formats, std::initializer_list<VkFormat> preferred)
{
	for (VkFormat candidate: preferred)
	{
		if (std::ranges::contains(formats, static_cast<int64_t>(candidate)))
			return static_cast<int64_t>(candidate);
	}
	throw failure("active runtime does not support a required swapchain format");
}

void poll_events(XrInstance instance, XrSession session, bool & running, bool & session_ready)
{
	for (;;)
	{
		XrEventDataBuffer event{XR_TYPE_EVENT_DATA_BUFFER};
		const XrResult result = xrPollEvent(instance, &event);
		if (result == XR_EVENT_UNAVAILABLE)
			return;
		check_xr(result, "xrPollEvent");
		if (event.type != XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED)
			continue;

		const auto * state = reinterpret_cast<const XrEventDataSessionStateChanged *>(&event);
		if (state->session != session)
			continue;
		if (state->state == XR_SESSION_STATE_READY)
		{
			XrSessionBeginInfo begin_info{
			        .type = XR_TYPE_SESSION_BEGIN_INFO,
			        .primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
			};
			check_xr(xrBeginSession(session, &begin_info), "xrBeginSession");
			session_ready = true;
		}
		else if (state->state == XR_SESSION_STATE_STOPPING)
		{
			if (session_ready)
				check_xr(xrEndSession(session), "xrEndSession");
			session_ready = false;
		}
		else if (state->state == XR_SESSION_STATE_EXITING || state->state == XR_SESSION_STATE_LOSS_PENDING)
			running = false;
	}
}

} // namespace

int main(int argc, char ** argv)
{
	try
	{
		const options opts = parse_options(argc, argv);
		const auto available_extensions = enumerate_extensions();
		const bool ext_available = has_extension(available_extensions, XR_EXT_FRAME_SYNTHESIS_EXTENSION_NAME);
		const bool fb_available = has_extension(available_extensions, XR_FB_SPACE_WARP_EXTENSION_NAME);
		const bool vulkan2_available = has_extension(available_extensions, XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME);
		std::cout << "OpenXR runtime extensions: XR_KHR_vulkan_enable2=" << (vulkan2_available ? 1 : 0)
		          << " XR_EXT_frame_synthesis=" << (ext_available ? 1 : 0)
		          << " XR_FB_space_warp=" << (fb_available ? 1 : 0) << '\n';

		bool use_ext = false;
		const bool use_metadata = opts.mode != options::metadata::none;
		switch (opts.mode)
		{
			case options::metadata::auto_select:
				if (ext_available)
					use_ext = true;
				else if (!fb_available)
					throw failure("active runtime exposes neither XR_EXT_frame_synthesis nor XR_FB_space_warp");
				break;
			case options::metadata::ext:
				if (!ext_available)
					throw failure("requested XR_EXT_frame_synthesis is unavailable");
				use_ext = true;
				break;
			case options::metadata::fb:
				if (!fb_available)
					throw failure("requested XR_FB_space_warp is unavailable");
				break;
			case options::metadata::none:
				break;
		}

		XrInstance instance = create_instance(available_extensions, use_metadata, use_ext);
		XrSystemId system = XR_NULL_SYSTEM_ID;
		XrSystemGetInfo system_info{
		        .type = XR_TYPE_SYSTEM_GET_INFO,
		        .formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY,
		};
		check_xr(xrGetSystem(instance, &system_info, &system), "xrGetSystem");

		uint32_t view_count = 0;
		check_xr(xrEnumerateViewConfigurationViews(instance,
		                                           system,
		                                           XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
		                                           0,
		                                           &view_count,
		                                           nullptr),
		         "xrEnumerateViewConfigurationViews(count)");
		if (view_count != 2)
			throw failure("harness requires a two-view primary stereo configuration");

		std::array<XrFrameSynthesisConfigViewEXT, 2> synthesis_config{
		        XrFrameSynthesisConfigViewEXT{XR_TYPE_FRAME_SYNTHESIS_CONFIG_VIEW_EXT},
		        XrFrameSynthesisConfigViewEXT{XR_TYPE_FRAME_SYNTHESIS_CONFIG_VIEW_EXT},
		};
		std::array<XrViewConfigurationView, 2> view_config{
		        XrViewConfigurationView{XR_TYPE_VIEW_CONFIGURATION_VIEW},
		        XrViewConfigurationView{XR_TYPE_VIEW_CONFIGURATION_VIEW},
		};
		if (use_ext)
		{
			view_config[0].next = &synthesis_config[0];
			view_config[1].next = &synthesis_config[1];
		}
		check_xr(xrEnumerateViewConfigurationViews(instance,
		                                           system,
		                                           XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
		                                           view_count,
		                                           &view_count,
		                                           view_config.data()),
		         "xrEnumerateViewConfigurationViews");

		std::cout << "WIVRN FSR OpenXR harness: metadata="
		          << (not use_metadata ? "none" : (use_ext ? "XR_EXT_frame_synthesis" : "XR_FB_space_warp"))
		          << " views=" << view_count << "\n";
		for (uint32_t i = 0; i < view_count; ++i)
		{
			std::cout << "  view[" << i << "] color=" << view_config[i].recommendedImageRectWidth << 'x'
			          << view_config[i].recommendedImageRectHeight;
			if (use_ext)
				std::cout << " motion=" << synthesis_config[i].recommendedMotionVectorImageRectWidth << 'x'
				          << synthesis_config[i].recommendedMotionVectorImageRectHeight;
			std::cout << '\n';
		}

		vulkan_context vulkan = create_vulkan(instance, system);
		const auto get_requirements =
		        get_xr_proc<PFN_xrGetVulkanGraphicsRequirements2KHR>(instance, "xrGetVulkanGraphicsRequirements2KHR");
		XrGraphicsRequirementsVulkan2KHR requirements{XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN2_KHR};
		check_xr(get_requirements(instance, system, &requirements), "xrGetVulkanGraphicsRequirements2KHR");

		XrGraphicsBindingVulkan2KHR binding{
		        .type = XR_TYPE_GRAPHICS_BINDING_VULKAN2_KHR,
		        .instance = vulkan.instance,
		        .physicalDevice = vulkan.physical_device,
		        .device = vulkan.device,
		        .queueFamilyIndex = vulkan.queue_family,
		        .queueIndex = 0,
		};
		XrSession session = XR_NULL_HANDLE;
		XrSessionCreateInfo session_info{
		        .type = XR_TYPE_SESSION_CREATE_INFO,
		        .next = &binding,
		        .systemId = system,
		};
		check_xr(xrCreateSession(instance, &session_info, &session), "xrCreateSession");

		std::vector<int64_t> formats;
		uint32_t format_count = 0;
		check_xr(xrEnumerateSwapchainFormats(session, 0, &format_count, nullptr),
		         "xrEnumerateSwapchainFormats(count)");
		formats.resize(format_count);
		check_xr(xrEnumerateSwapchainFormats(session, format_count, &format_count, formats.data()),
		         "xrEnumerateSwapchainFormats");
		const int64_t color_format = choose_format(formats, {VK_FORMAT_R8G8B8A8_SRGB, VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_B8G8R8A8_SRGB, VK_FORMAT_B8G8R8A8_UNORM});
		int64_t motion_format = 0;
		int64_t depth_format = 0;
		if (use_metadata)
		{
			motion_format = choose_format(formats, {VK_FORMAT_R16G16_SFLOAT, VK_FORMAT_R16G16B16A16_SFLOAT, static_cast<VkFormat>(color_format)});
			depth_format = choose_format(formats, {VK_FORMAT_D32_SFLOAT, VK_FORMAT_D16_UNORM, VK_FORMAT_D24_UNORM_S8_UINT, VK_FORMAT_D32_SFLOAT_S8_UINT});
		}

		std::array<swapchain, 2> color;
		std::array<swapchain, 2> motion;
		std::array<swapchain, 2> depth;
		for (uint32_t i = 0; i < view_count; ++i)
		{
			color[i] = create_swapchain(session,
			                            view_config[i].recommendedImageRectWidth,
			                            view_config[i].recommendedImageRectHeight,
			                            color_format,
			                            XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT);
			if (use_metadata)
			{
				motion[i] = create_swapchain(session,
				                             view_config[i].recommendedImageRectWidth,
				                             view_config[i].recommendedImageRectHeight,
				                             motion_format,
				                             XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT);
				depth[i] = create_swapchain(session,
				                            view_config[i].recommendedImageRectWidth,
				                            view_config[i].recommendedImageRectHeight,
				                            depth_format,
				                            XR_SWAPCHAIN_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT);
			}
		}

		XrPosef identity_pose{{0, 0, 0, 1}, {0, 0, 0}};
		XrSpace space = XR_NULL_HANDLE;
		XrReferenceSpaceCreateInfo space_info{
		        .type = XR_TYPE_REFERENCE_SPACE_CREATE_INFO,
		        .referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL,
		        .poseInReferenceSpace = identity_pose,
		};
		XrResult space_result = xrCreateReferenceSpace(session, &space_info, &space);
		if (XR_FAILED(space_result))
		{
			space_info.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
			check_xr(xrCreateReferenceSpace(session, &space_info, &space), "xrCreateReferenceSpace");
		}

		bool running = true;
		bool session_ready = false;
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(opts.timeout_ms);
		uint32_t submitted = 0;
		while (running && submitted < opts.frames)
		{
			poll_events(instance, session, running, session_ready);
			if (!session_ready)
			{
				if (std::chrono::steady_clock::now() > deadline)
					throw failure("timed out waiting for XR_SESSION_STATE_READY");
				std::this_thread::sleep_for(std::chrono::milliseconds(10));
				continue;
			}

			XrFrameState frame_state{XR_TYPE_FRAME_STATE};
			XrFrameWaitInfo wait_info{XR_TYPE_FRAME_WAIT_INFO};
			check_xr(xrWaitFrame(session, &wait_info, &frame_state), "xrWaitFrame");
			XrFrameBeginInfo begin_info{XR_TYPE_FRAME_BEGIN_INFO};
			check_xr(xrBeginFrame(session, &begin_info), "xrBeginFrame");

			std::array<XrCompositionLayerProjectionView, 2> projection_views{
			        XrCompositionLayerProjectionView{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW},
			        XrCompositionLayerProjectionView{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW},
			};
			std::array<XrView, 2> located_views{
			        XrView{XR_TYPE_VIEW},
			        XrView{XR_TYPE_VIEW},
			};
			XrViewLocateInfo locate_info{
			        .type = XR_TYPE_VIEW_LOCATE_INFO,
			        .viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
			        .displayTime = frame_state.predictedDisplayTime,
			        .space = space,
			};
			XrViewState view_state{XR_TYPE_VIEW_STATE};
			uint32_t located_count = 0;
			check_xr(xrLocateViews(session,
			                       &locate_info,
			                       &view_state,
			                       located_views.size(),
			                       &located_count,
			                       located_views.data()),
			         "xrLocateViews");
			if (located_count != view_count)
				throw failure("xrLocateViews did not return both stereo views");

			std::vector<swapchain *> all_swapchains;
			for (uint32_t i = 0; i < view_count; ++i)
			{
				all_swapchains.push_back(&color[i]);
				if (use_metadata)
				{
					all_swapchains.push_back(&motion[i]);
					all_swapchains.push_back(&depth[i]);
				}
			}
			acquire_and_release(all_swapchains);

			std::array<XrFrameSynthesisInfoEXT, 2> synthesis_info{
			        XrFrameSynthesisInfoEXT{XR_TYPE_FRAME_SYNTHESIS_INFO_EXT},
			        XrFrameSynthesisInfoEXT{XR_TYPE_FRAME_SYNTHESIS_INFO_EXT},
			};
			std::array<XrCompositionLayerSpaceWarpInfoFB, 2> space_warp_info{
			        XrCompositionLayerSpaceWarpInfoFB{XR_TYPE_COMPOSITION_LAYER_SPACE_WARP_INFO_FB},
			        XrCompositionLayerSpaceWarpInfoFB{XR_TYPE_COMPOSITION_LAYER_SPACE_WARP_INFO_FB},
			};
			for (uint32_t i = 0; i < view_count; ++i)
			{
				const XrRect2Di rect{{0, 0}, {static_cast<int32_t>(color[i].width), static_cast<int32_t>(color[i].height)}};
				projection_views[i].pose = located_views[i].pose;
				projection_views[i].fov = located_views[i].fov;
				projection_views[i].subImage = XrSwapchainSubImage{color[i].handle, rect, 0};

				if (use_ext)
				{
					synthesis_info[i].motionVectorSubImage = XrSwapchainSubImage{motion[i].handle, rect, 0};
					synthesis_info[i].motionVectorScale = {1, 1, 1, 1};
					synthesis_info[i].motionVectorOffset = {0, 0, 0, 0};
					synthesis_info[i].appSpaceDeltaPose = identity_pose;
					synthesis_info[i].depthSubImage = XrSwapchainSubImage{depth[i].handle, rect, 0};
					synthesis_info[i].minDepth = 0.0f;
					synthesis_info[i].maxDepth = 1.0f;
					synthesis_info[i].nearZ = 0.05f;
					synthesis_info[i].farZ = 100.0f;
					projection_views[i].next = &synthesis_info[i];
				}
				else if (use_metadata)
				{
					space_warp_info[i].motionVectorSubImage = XrSwapchainSubImage{motion[i].handle, rect, 0};
					space_warp_info[i].appSpaceDeltaPose = identity_pose;
					space_warp_info[i].depthSubImage = XrSwapchainSubImage{depth[i].handle, rect, 0};
					space_warp_info[i].minDepth = 0.0f;
					space_warp_info[i].maxDepth = 1.0f;
					space_warp_info[i].nearZ = 0.05f;
					space_warp_info[i].farZ = 100.0f;
					projection_views[i].next = &space_warp_info[i];
				}
			}

			XrCompositionLayerProjection projection_layer{
			        .type = XR_TYPE_COMPOSITION_LAYER_PROJECTION,
			        .space = space,
			        .viewCount = view_count,
			        .views = projection_views.data(),
			};
			const XrCompositionLayerBaseHeader * layers[] = {
			        reinterpret_cast<const XrCompositionLayerBaseHeader *>(&projection_layer),
			};
			XrFrameEndInfo end_info{
			        .type = XR_TYPE_FRAME_END_INFO,
			        .displayTime = frame_state.predictedDisplayTime,
			        .environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE,
			        .layerCount = frame_state.shouldRender ? 1u : 0u,
			        .layers = frame_state.shouldRender ? layers : nullptr,
			};
			check_xr(xrEndFrame(session, &end_info), "xrEndFrame");
			if (frame_state.shouldRender)
			{
				++submitted;
				if (submitted <= 5 || submitted % 100 == 0 || submitted == opts.frames)
				{
					std::cout << "submitted frame " << submitted << "/" << opts.frames
					          << " views=2 metadata="
					          << (not use_metadata ? "none" : (use_ext ? "EXT" : "FB"))
					          << " payload=structural-only\n";
				}
			}
		}

		if (session_ready)
		{
			check_xr(xrRequestExitSession(session), "xrRequestExitSession");
			const auto stop_deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(opts.timeout_ms);
			while (session_ready && std::chrono::steady_clock::now() <= stop_deadline)
			{
				poll_events(instance, session, running, session_ready);
				if (session_ready)
					std::this_thread::sleep_for(std::chrono::milliseconds(10));
			}
			if (session_ready)
				throw failure("timed out waiting for XR_SESSION_STATE_STOPPING");
		}
		for (auto & chain: color)
			chain.reset();
		for (auto & chain: motion)
			chain.reset();
		for (auto & chain: depth)
			chain.reset();
		xrDestroySpace(space);
		xrDestroySession(session);
		xrDestroyInstance(instance);
		return 0;
	}
	catch (const std::exception & error)
	{
		std::cerr << "wivrn-fsr-openxr-harness: " << error.what() << '\n';
		return 1;
	}
}
