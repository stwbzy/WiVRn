#include "mocktail/vr/openxr_backend.h"
#include <SDL3/SDL.h>
#include <dlfcn.h>
#include <algorithm>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {
SDL_Window* window = nullptr;
void VkCheck(VkResult r, const char* operation) {
    if (r != VK_SUCCESS) throw std::runtime_error(std::string(operation) + " result=" + std::to_string(r));
}
template<class T> T Symbol(void* library, const char* name) {
    auto* symbol = dlsym(library, name);
    if (!symbol) throw std::runtime_error(std::string("Missing adapter symbol: ") + name);
    return reinterpret_cast<T>(symbol);
}
template<class T> T Proc(PFN_vkGetInstanceProcAddr get, VkInstance instance, const char* name) {
    auto symbol = get(instance, name);
    if (!symbol) throw std::runtime_error(std::string("Missing Vulkan proc: ") + name);
    return reinterpret_cast<T>(symbol);
}
struct SdlContext {
    ~SdlContext() { if (window) SDL_DestroyWindow(window); window = nullptr; SDL_Quit(); }
};
struct VkContext {
    VkInstance instance = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    PFN_vkDestroyInstance destroy_instance = nullptr;
    PFN_vkDestroyDevice destroy_device = nullptr;
    ~VkContext() {
        if (device && destroy_device) destroy_device(device, nullptr);
        if (instance && destroy_instance) destroy_instance(instance, nullptr);
    }
};
}

// Host callbacks required by the real Android->SDL WSI adapter.
extern "C" void* mocktail_window_backend_window() { return window; }
extern "C" bool mocktail_window_uses_direct_vulkan() { return window != nullptr; }

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "Usage: wivrn-roblox-host-smoke PATH_TO_ADAPTER_SO\n";
        return 1;
    }
    try {
        SdlContext sdl;
        if (!SDL_Init(SDL_INIT_VIDEO)) throw std::runtime_error(SDL_GetError());
        window = SDL_CreateWindow("WiVRn VR host smoke (not Roblox)", 640, 480,
                                  SDL_WINDOW_VULKAN | SDL_WINDOW_HIDDEN);
        if (!window) throw std::runtime_error(SDL_GetError());
        mocktail::vr::OpenXrBackend backend;
        auto armed = backend.Arm();
        if (!armed) throw std::runtime_error(armed.message());
        // Retain the module until process exit: upstream caches its callback addresses.
        void* adapter = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
        if (!adapter) throw std::runtime_error(dlerror());
        auto get = Symbol<PFN_vkGetInstanceProcAddr>(adapter, "vkGetInstanceProcAddr");
        auto create = Symbol<PFN_vkCreateInstance>(adapter, "vkCreateInstance");
        VkContext vk;
        VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
        app.pApplicationName = "WiVRn Roblox host smoke";
        app.apiVersion = VK_API_VERSION_1_1;
        const char* android_extension = "VK_KHR_android_surface";
        VkInstanceCreateInfo info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        info.pApplicationInfo = &app;
        info.enabledExtensionCount = 1;
        info.ppEnabledExtensionNames = &android_extension;
        VkCheck(create(&info, nullptr, &vk.instance), "adapter vkCreateInstance");
        vk.destroy_instance = Proc<PFN_vkDestroyInstance>(get, vk.instance, "vkDestroyInstance");
        auto enumerate = Proc<PFN_vkEnumeratePhysicalDevices>(get, vk.instance, "vkEnumeratePhysicalDevices");
        uint32_t count = 0;
        VkCheck(enumerate(vk.instance, &count, nullptr), "enumerate GPU count");
        if (!count) throw std::runtime_error("No GPUs");
        std::vector<VkPhysicalDevice> devices(count);
        VkCheck(enumerate(vk.instance, &count, devices.data()), "enumerate GPUs");
        auto properties = Proc<PFN_vkGetPhysicalDeviceProperties>(get, vk.instance, "vkGetPhysicalDeviceProperties");
        auto families = Proc<PFN_vkGetPhysicalDeviceQueueFamilyProperties>(get, vk.instance, "vkGetPhysicalDeviceQueueFamilyProperties");
        auto create_device = Proc<PFN_vkCreateDevice>(get, vk.instance, "vkCreateDevice");
        auto physical = devices.front();
        uint32_t family_count = 0;
        families(physical, &family_count, nullptr);
        std::vector<VkQueueFamilyProperties> queues(family_count);
        families(physical, &family_count, queues.data());
        auto graphics = std::find_if(queues.begin(), queues.end(), [](const auto& q) {
            return q.queueCount && (q.queueFlags & VK_QUEUE_GRAPHICS_BIT);
        });
        if (graphics == queues.end()) throw std::runtime_error("No graphics queue");
        uint32_t family = static_cast<uint32_t>(graphics - queues.begin());
        float priority = 1;
        VkDeviceQueueCreateInfo queue_info{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        queue_info.queueFamilyIndex = family; queue_info.queueCount = 1; queue_info.pQueuePriorities = &priority;
        const char* swapchain_extension = VK_KHR_SWAPCHAIN_EXTENSION_NAME;
        VkDeviceCreateInfo device_info{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
        device_info.queueCreateInfoCount = 1; device_info.pQueueCreateInfos = &queue_info;
        device_info.enabledExtensionCount = 1; device_info.ppEnabledExtensionNames = &swapchain_extension;
        VkCheck(create_device(physical, &device_info, nullptr, &vk.device), "adapter vkCreateDevice");
        auto device_get = Symbol<PFN_vkGetDeviceProcAddr>(adapter, "vkGetDeviceProcAddr");
        vk.destroy_device = reinterpret_cast<PFN_vkDestroyDevice>(device_get(vk.device, "vkDestroyDevice"));
        auto get_queue = reinterpret_cast<PFN_vkGetDeviceQueue>(device_get(vk.device, "vkGetDeviceQueue"));
        if (!vk.destroy_device || !get_queue) throw std::runtime_error("Missing device dispatch");
        VkQueue queue = VK_NULL_HANDLE;
        get_queue(vk.device, family, 0, &queue);
        VkPhysicalDeviceProperties gpu{};
        properties(physical, &gpu);
        std::cout << "android_wsi_adapter=1 gpu=" << gpu.deviceName
                  << " runtime=" << backend.runtime_name()
                  << " queue_valid=" << (queue != VK_NULL_HANDLE)
                  << " roblox_loaded=0 submitted_frames=" << backend.submitted_frames() << '\n';
        backend.Disarm();
        return queue ? 0 : 1;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
