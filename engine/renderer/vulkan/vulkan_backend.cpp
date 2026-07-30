#include "engine/renderer/vulkan/vulkan_backend.hpp"

#if HEARTSTEAD_HAS_VULKAN
#include "engine/core/logging.hpp"
#include "engine/renderer/memory/staging_ring.hpp"
#include "engine/renderer/rhi/render_frame_plan.hpp"
#include "engine/renderer/vulkan/vulkan_common.hpp"
#include "engine/renderer/vulkan/vulkan_frame_resources.hpp"

#include <vulkan/vulkan.h>
#if HEARTSTEAD_HAS_X11
#include <X11/Xlib.h>
#ifdef Status
#undef Status
#endif
#include <vulkan/vulkan_xlib.h>
#ifdef Status
#undef Status
#endif
#endif

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <deque>
#include <initializer_list>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>
#endif

namespace heartstead::renderer::vulkan {

#if HEARTSTEAD_HAS_VULKAN

namespace {

// Shared with the other Vulkan subsystems; see vulkan_common.hpp.
using detail::find_memory_type;
using detail::vk_result_name;
using detail::vulkan_image_format;

[[nodiscard]] bool requests_x11_surface(const rhi::RenderDeviceDesc& desc) noexcept {
    return desc.native_window.has_value() &&
           desc.native_window->system == platform::NativeWindowSystem::x11;
}

[[nodiscard]] std::vector<const char*>
required_instance_extensions(const rhi::RenderDeviceDesc& desc, bool enable_debug_utils) {
    std::vector<const char*> extensions;
    if (requests_x11_surface(desc)) {
#if HEARTSTEAD_HAS_X11
        extensions.push_back(VK_KHR_SURFACE_EXTENSION_NAME);
        extensions.push_back(VK_KHR_XLIB_SURFACE_EXTENSION_NAME);
#endif
    }
    if (enable_debug_utils) {
        extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    }
    return extensions;
}

[[nodiscard]] bool instance_layer_available(std::string_view requested_name) {
    std::uint32_t count = 0;
    auto result = vkEnumerateInstanceLayerProperties(&count, nullptr);
    if (result != VK_SUCCESS) {
        return false;
    }
    std::vector<VkLayerProperties> layers(count);
    result = vkEnumerateInstanceLayerProperties(&count, layers.data());
    if (result != VK_SUCCESS && result != VK_INCOMPLETE) {
        return false;
    }
    layers.resize(count);
    return std::ranges::any_of(layers, [requested_name](const VkLayerProperties& layer) {
        return std::string_view(layer.layerName) == requested_name;
    });
}

[[nodiscard]] bool instance_extension_available(std::string_view requested_name) {
    std::uint32_t count = 0;
    auto result = vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr);
    if (result != VK_SUCCESS) {
        return false;
    }
    std::vector<VkExtensionProperties> extensions(count);
    result = vkEnumerateInstanceExtensionProperties(nullptr, &count, extensions.data());
    if (result != VK_SUCCESS && result != VK_INCOMPLETE) {
        return false;
    }
    extensions.resize(count);
    return std::ranges::any_of(
        extensions, [requested_name](const VkExtensionProperties& extension) {
            return std::string_view(extension.extensionName) == requested_name;
        });
}

VKAPI_ATTR VkBool32 VKAPI_CALL debug_messenger_callback(
    VkDebugUtilsMessageSeverityFlagBitsEXT severity, VkDebugUtilsMessageTypeFlagsEXT,
    const VkDebugUtilsMessengerCallbackDataEXT* callback_data, void*) {
    auto level = core::LogLevel::debug;
    if ((severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) != 0) {
        level = core::LogLevel::error;
    } else if ((severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) != 0) {
        level = core::LogLevel::warning;
    } else if ((severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT) != 0) {
        level = core::LogLevel::trace;
    }
    const auto* message = callback_data == nullptr ? nullptr : callback_data->pMessage;
    core::log(level, "Vulkan validation: " +
                         std::string(message == nullptr ? "message unavailable" : message));
    return VK_FALSE;
}

[[nodiscard]] VkDebugUtilsMessengerCreateInfoEXT make_debug_messenger_info() noexcept {
    VkDebugUtilsMessengerCreateInfoEXT info{};
    info.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
    info.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                           VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    info.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                       VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                       VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    info.pfnUserCallback = debug_messenger_callback;
    return info;
}

struct VulkanInstanceResource {
    VkInstance instance = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT debug_messenger = VK_NULL_HANDLE;
    bool validation_enabled = false;
    bool debug_utils_enabled = false;
};

void destroy_instance_resource(VulkanInstanceResource& resource) noexcept {
    if (resource.debug_messenger != VK_NULL_HANDLE && resource.instance != VK_NULL_HANDLE) {
        const auto destroy_debug_messenger = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(resource.instance, "vkDestroyDebugUtilsMessengerEXT"));
        if (destroy_debug_messenger != nullptr) {
            destroy_debug_messenger(resource.instance, resource.debug_messenger, nullptr);
        }
        resource.debug_messenger = VK_NULL_HANDLE;
    }
    if (resource.instance != VK_NULL_HANDLE) {
        vkDestroyInstance(resource.instance, nullptr);
        resource.instance = VK_NULL_HANDLE;
    }
}

[[nodiscard]] core::Result<VulkanInstanceResource>
create_instance(const rhi::RenderDeviceDesc& desc) {
    constexpr std::string_view validation_layer = "VK_LAYER_KHRONOS_validation";
    const auto validation_available = instance_layer_available(validation_layer);
    const auto enable_validation = desc.enable_validation && validation_available;
    if (desc.enable_validation && !validation_available) {
        core::log(core::LogLevel::info,
                  "VK_LAYER_KHRONOS_validation is not installed; continuing without Vulkan "
                  "validation");
    }
    const auto debug_utils_available =
        instance_extension_available(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    const auto enable_debug_utils = debug_utils_available;
    if (!debug_utils_available) {
        core::log(core::LogLevel::info,
                  "VK_EXT_debug_utils is unavailable; Vulkan command labels are disabled");
    }

    VkApplicationInfo app_info{};
    app_info.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app_info.pApplicationName = desc.application_name.c_str();
    app_info.applicationVersion = VK_MAKE_VERSION(0, 1, 0);
    app_info.pEngineName = "Heartstead";
    app_info.engineVersion = VK_MAKE_VERSION(0, 1, 0);
    // Dynamic rendering and synchronization2 are core in 1.3. The renderer targets that
    // baseline so render passes and framebuffers stay out of the frame graph.
    app_info.apiVersion = VK_API_VERSION_1_3;

    VkInstanceCreateInfo instance_info{};
    instance_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    instance_info.pApplicationInfo = &app_info;
    const auto extensions = required_instance_extensions(desc, enable_debug_utils);
    instance_info.enabledExtensionCount = static_cast<std::uint32_t>(extensions.size());
    instance_info.ppEnabledExtensionNames = extensions.empty() ? nullptr : extensions.data();
    const std::array<const char*, 1> layers{"VK_LAYER_KHRONOS_validation"};
    if (enable_validation) {
        instance_info.enabledLayerCount = static_cast<std::uint32_t>(layers.size());
        instance_info.ppEnabledLayerNames = layers.data();
    }
    auto debug_info = make_debug_messenger_info();
    if (enable_debug_utils) {
        instance_info.pNext = &debug_info;
    }

    VulkanInstanceResource resource;
    const auto result = vkCreateInstance(&instance_info, nullptr, &resource.instance);
    if (result != VK_SUCCESS) {
        return core::Result<VulkanInstanceResource>::failure(
            "renderer.vulkan_instance_failed",
            "failed to create Vulkan instance: " + std::string(vk_result_name(result)));
    }
    resource.validation_enabled = enable_validation;
    resource.debug_utils_enabled = enable_debug_utils;
    if (enable_debug_utils) {
        const auto create_debug_messenger = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(resource.instance, "vkCreateDebugUtilsMessengerEXT"));
        if (create_debug_messenger != nullptr &&
            create_debug_messenger(resource.instance, &debug_info, nullptr,
                                   &resource.debug_messenger) == VK_SUCCESS) {
        } else {
            core::log(core::LogLevel::warning,
                      "Vulkan debug utils extension is present but the optional messenger failed");
        }
    }
    return core::Result<VulkanInstanceResource>::success(std::move(resource));
}

[[nodiscard]] core::Result<VkSurfaceKHR>
create_native_surface(VkInstance instance, const platform::NativeWindowHandle& handle) {
    if (handle.system != platform::NativeWindowSystem::x11 || handle.display == nullptr ||
        handle.window == 0) {
        return core::Result<VkSurfaceKHR>::failure(
            "renderer.vulkan_invalid_native_window",
            "Vulkan native window handle must name a valid X11 display and window");
    }

#if HEARTSTEAD_HAS_X11
    VkXlibSurfaceCreateInfoKHR surface_info{};
    surface_info.sType = VK_STRUCTURE_TYPE_XLIB_SURFACE_CREATE_INFO_KHR;
    surface_info.dpy = static_cast<Display*>(handle.display);
    surface_info.window = static_cast<::Window>(handle.window);

    VkSurfaceKHR surface = VK_NULL_HANDLE;
    const auto result = vkCreateXlibSurfaceKHR(instance, &surface_info, nullptr, &surface);
    if (result != VK_SUCCESS) {
        return core::Result<VkSurfaceKHR>::failure("renderer.vulkan_surface_failed",
                                                   "failed to create Vulkan X11 surface: " +
                                                       std::string(vk_result_name(result)));
    }
    return core::Result<VkSurfaceKHR>::success(surface);
#else
    (void)instance;
    return core::Result<VkSurfaceKHR>::failure("renderer.vulkan_x11_surface_unavailable",
                                               "Vulkan X11 surface support is not compiled in");
#endif
}

[[nodiscard]] bool has_graphics_physical_device(VkInstance instance) {
    std::uint32_t device_count = 0;
    auto result = vkEnumeratePhysicalDevices(instance, &device_count, nullptr);
    if (result != VK_SUCCESS || device_count == 0) {
        return false;
    }

    std::vector<VkPhysicalDevice> devices(device_count);
    result = vkEnumeratePhysicalDevices(instance, &device_count, devices.data());
    if (result != VK_SUCCESS && result != VK_INCOMPLETE) {
        return false;
    }
    devices.resize(device_count);

    for (const auto physical_device : devices) {
        std::uint32_t queue_family_count = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(physical_device, &queue_family_count, nullptr);
        std::vector<VkQueueFamilyProperties> queue_families(queue_family_count);
        vkGetPhysicalDeviceQueueFamilyProperties(physical_device, &queue_family_count,
                                                 queue_families.data());
        const auto graphics_family =
            std::ranges::find_if(queue_families, [](const VkQueueFamilyProperties& family) {
                return (family.queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0 && family.queueCount > 0;
            });
        if (graphics_family != queue_families.end()) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] bool probe_backend() noexcept {
    VkApplicationInfo app_info{};
    app_info.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app_info.pApplicationName = "Heartstead Vulkan Probe";
    app_info.applicationVersion = VK_MAKE_VERSION(0, 1, 0);
    app_info.pEngineName = "Heartstead";
    app_info.engineVersion = VK_MAKE_VERSION(0, 1, 0);
    // Dynamic rendering and synchronization2 are core in 1.3. The renderer targets that
    // baseline so render passes and framebuffers stay out of the frame graph.
    app_info.apiVersion = VK_API_VERSION_1_3;

    VkInstanceCreateInfo instance_info{};
    instance_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    instance_info.pApplicationInfo = &app_info;

    VkInstance instance = VK_NULL_HANDLE;
    if (vkCreateInstance(&instance_info, nullptr, &instance) != VK_SUCCESS) {
        return false;
    }
    bool has_physical_device = false;
    try {
        has_physical_device = has_graphics_physical_device(instance);
    } catch (...) {
        has_physical_device = false;
    }
    vkDestroyInstance(instance, nullptr);
    return has_physical_device;
}

struct SelectedPhysicalDevice {
    VkPhysicalDevice physical_device = VK_NULL_HANDLE;
    VkPhysicalDeviceProperties properties{};
    std::uint32_t graphics_queue_family = 0;
    std::uint32_t timestamp_valid_bits = 0;
    VkFormat depth_format = VK_FORMAT_UNDEFINED;
};

struct SwapchainSupport {
    VkSurfaceCapabilitiesKHR capabilities{};
    std::vector<VkSurfaceFormatKHR> formats;
    std::vector<VkPresentModeKHR> present_modes;
};

[[nodiscard]] bool physical_device_supports_extension(VkPhysicalDevice physical_device,
                                                      const char* extension_name) {
    std::uint32_t extension_count = 0;
    auto result =
        vkEnumerateDeviceExtensionProperties(physical_device, nullptr, &extension_count, nullptr);
    if (result != VK_SUCCESS) {
        return false;
    }

    std::vector<VkExtensionProperties> extensions(extension_count);
    result = vkEnumerateDeviceExtensionProperties(physical_device, nullptr, &extension_count,
                                                  extensions.data());
    if (result != VK_SUCCESS && result != VK_INCOMPLETE) {
        return false;
    }
    extensions.resize(extension_count);

    return std::ranges::any_of(
        extensions, [extension_name](const VkExtensionProperties& extension) {
            return std::string_view(extension.extensionName) == extension_name;
        });
}

// The frame graph records with vkCmdBeginRendering, so a device without dynamic rendering
// cannot execute a frame at all and must not be selected.
[[nodiscard]] bool physical_device_supports_dynamic_rendering(VkPhysicalDevice physical_device) {
    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(physical_device, &properties);
    if (properties.apiVersion < VK_API_VERSION_1_3) {
        return false;
    }

    VkPhysicalDeviceVulkan13Features features_13{};
    features_13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
    VkPhysicalDeviceFeatures2 features{};
    features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    features.pNext = &features_13;
    vkGetPhysicalDeviceFeatures2(physical_device, &features);
    return features_13.dynamicRendering == VK_TRUE;
}

[[nodiscard]] core::Result<SwapchainSupport>
query_swapchain_support(VkPhysicalDevice physical_device, VkSurfaceKHR surface) {
    SwapchainSupport support;
    auto result =
        vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical_device, surface, &support.capabilities);
    if (result != VK_SUCCESS) {
        return core::Result<SwapchainSupport>::failure(
            "renderer.vulkan_surface_capabilities_failed",
            "failed to query Vulkan surface capabilities: " + std::string(vk_result_name(result)));
    }

    std::uint32_t format_count = 0;
    result = vkGetPhysicalDeviceSurfaceFormatsKHR(physical_device, surface, &format_count, nullptr);
    if (result != VK_SUCCESS) {
        return core::Result<SwapchainSupport>::failure("renderer.vulkan_surface_formats_failed",
                                                       "failed to query Vulkan surface formats: " +
                                                           std::string(vk_result_name(result)));
    }
    support.formats.resize(format_count);
    if (format_count > 0) {
        result = vkGetPhysicalDeviceSurfaceFormatsKHR(physical_device, surface, &format_count,
                                                      support.formats.data());
        if (result != VK_SUCCESS && result != VK_INCOMPLETE) {
            return core::Result<SwapchainSupport>::failure(
                "renderer.vulkan_surface_formats_failed",
                "failed to query Vulkan surface formats: " + std::string(vk_result_name(result)));
        }
        support.formats.resize(format_count);
    }

    std::uint32_t present_mode_count = 0;
    result = vkGetPhysicalDeviceSurfacePresentModesKHR(physical_device, surface,
                                                       &present_mode_count, nullptr);
    if (result != VK_SUCCESS) {
        return core::Result<SwapchainSupport>::failure("renderer.vulkan_present_modes_failed",
                                                       "failed to query Vulkan present modes: " +
                                                           std::string(vk_result_name(result)));
    }
    support.present_modes.resize(present_mode_count);
    if (present_mode_count > 0) {
        result = vkGetPhysicalDeviceSurfacePresentModesKHR(
            physical_device, surface, &present_mode_count, support.present_modes.data());
        if (result != VK_SUCCESS && result != VK_INCOMPLETE) {
            return core::Result<SwapchainSupport>::failure(
                "renderer.vulkan_present_modes_failed",
                "failed to query Vulkan present modes: " + std::string(vk_result_name(result)));
        }
        support.present_modes.resize(present_mode_count);
    }

    return core::Result<SwapchainSupport>::success(std::move(support));
}

[[nodiscard]] VkSurfaceFormatKHR
choose_surface_format(const std::vector<VkSurfaceFormatKHR>& formats) noexcept {
    const auto preferred = std::ranges::find_if(formats, [](const VkSurfaceFormatKHR& format) {
        return (format.format == VK_FORMAT_B8G8R8A8_UNORM ||
                format.format == VK_FORMAT_R8G8B8A8_UNORM) &&
               format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    });
    return preferred != formats.end() ? *preferred : formats.front();
}

[[nodiscard]] VkPresentModeKHR
choose_present_mode(const std::vector<VkPresentModeKHR>& present_modes,
                    rhi::PresentMode requested) noexcept {
    const auto has_mode = [&present_modes](VkPresentModeKHR mode) {
        return std::ranges::find(present_modes, mode) != present_modes.end();
    };

    if (requested == rhi::PresentMode::mailbox && has_mode(VK_PRESENT_MODE_MAILBOX_KHR)) {
        return VK_PRESENT_MODE_MAILBOX_KHR;
    }
    if (requested == rhi::PresentMode::immediate && has_mode(VK_PRESENT_MODE_IMMEDIATE_KHR)) {
        return VK_PRESENT_MODE_IMMEDIATE_KHR;
    }
    return VK_PRESENT_MODE_FIFO_KHR;
}

[[nodiscard]] VkExtent2D choose_swapchain_extent(const VkSurfaceCapabilitiesKHR& capabilities,
                                                 rhi::RenderExtent requested) noexcept {
    if (capabilities.currentExtent.width != std::numeric_limits<std::uint32_t>::max()) {
        return capabilities.currentExtent;
    }

    VkExtent2D extent{requested.width, requested.height};
    extent.width = std::clamp(extent.width, capabilities.minImageExtent.width,
                              capabilities.maxImageExtent.width);
    extent.height = std::clamp(extent.height, capabilities.minImageExtent.height,
                               capabilities.maxImageExtent.height);
    return extent;
}

[[nodiscard]] VkFormat choose_supported_depth_format(VkPhysicalDevice physical_device) noexcept {
    constexpr std::array<VkFormat, 3> candidates{
        VK_FORMAT_D32_SFLOAT,
        VK_FORMAT_D32_SFLOAT_S8_UINT,
        VK_FORMAT_D24_UNORM_S8_UINT,
    };
    for (const auto format : candidates) {
        VkFormatProperties properties{};
        vkGetPhysicalDeviceFormatProperties(physical_device, format, &properties);
        if ((properties.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) !=
            0) {
            return format;
        }
    }
    return VK_FORMAT_UNDEFINED;
}

[[nodiscard]] core::Result<SelectedPhysicalDevice> select_physical_device(VkInstance instance,
                                                                          VkSurfaceKHR surface) {
    std::uint32_t device_count = 0;
    auto result = vkEnumeratePhysicalDevices(instance, &device_count, nullptr);
    if (result != VK_SUCCESS || device_count == 0) {
        return core::Result<SelectedPhysicalDevice>::failure(
            "renderer.vulkan_no_device",
            "no Vulkan physical device is available: " + std::string(vk_result_name(result)));
    }

    std::vector<VkPhysicalDevice> devices(device_count);
    result = vkEnumeratePhysicalDevices(instance, &device_count, devices.data());
    if (result != VK_SUCCESS && result != VK_INCOMPLETE) {
        return core::Result<SelectedPhysicalDevice>::failure(
            "renderer.vulkan_enumerate_devices_failed",
            "failed to enumerate Vulkan physical devices: " + std::string(vk_result_name(result)));
    }
    devices.resize(device_count);

    for (const auto physical_device : devices) {
        std::uint32_t queue_family_count = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(physical_device, &queue_family_count, nullptr);
        std::vector<VkQueueFamilyProperties> queue_families(queue_family_count);
        vkGetPhysicalDeviceQueueFamilyProperties(physical_device, &queue_family_count,
                                                 queue_families.data());

        auto graphics_family = queue_families.end();
        for (auto family = queue_families.begin(); family != queue_families.end(); ++family) {
            if ((family->queueFlags & VK_QUEUE_GRAPHICS_BIT) == 0 || family->queueCount == 0) {
                continue;
            }
            if (surface != VK_NULL_HANDLE) {
                VkBool32 surface_supported = VK_FALSE;
                const auto support_result = vkGetPhysicalDeviceSurfaceSupportKHR(
                    physical_device,
                    static_cast<std::uint32_t>(std::distance(queue_families.begin(), family)),
                    surface, &surface_supported);
                if (support_result != VK_SUCCESS || surface_supported != VK_TRUE) {
                    continue;
                }
            }
            graphics_family = family;
            break;
        }
        if (graphics_family == queue_families.end()) {
            continue;
        }
        if (!physical_device_supports_dynamic_rendering(physical_device)) {
            continue;
        }
        if (surface != VK_NULL_HANDLE) {
            if (!physical_device_supports_extension(physical_device,
                                                    VK_KHR_SWAPCHAIN_EXTENSION_NAME)) {
                continue;
            }
            auto swapchain_support = query_swapchain_support(physical_device, surface);
            if (!swapchain_support || swapchain_support.value().formats.empty() ||
                swapchain_support.value().present_modes.empty() ||
                (swapchain_support.value().capabilities.supportedUsageFlags &
                 VK_IMAGE_USAGE_TRANSFER_DST_BIT) == 0) {
                continue;
            }
        }

        SelectedPhysicalDevice selected;
        selected.physical_device = physical_device;
        vkGetPhysicalDeviceProperties(physical_device, &selected.properties);
        selected.depth_format = choose_supported_depth_format(physical_device);
        if (selected.depth_format == VK_FORMAT_UNDEFINED) {
            continue;
        }
        selected.graphics_queue_family =
            static_cast<std::uint32_t>(std::distance(queue_families.begin(), graphics_family));
        selected.timestamp_valid_bits = graphics_family->timestampValidBits;
        return core::Result<SelectedPhysicalDevice>::success(selected);
    }

    return core::Result<SelectedPhysicalDevice>::failure(
        surface == VK_NULL_HANDLE ? "renderer.vulkan_no_graphics_queue"
                                  : "renderer.vulkan_no_surface_queue",
        surface == VK_NULL_HANDLE
            ? "no Vulkan physical device exposes a graphics queue family with Vulkan 1.3 dynamic "
              "rendering"
            : "no Vulkan physical device exposes a graphics queue family with Vulkan 1.3 dynamic "
              "rendering for the window surface");
}

[[nodiscard]] core::Result<VkDevice> create_logical_device(VkPhysicalDevice physical_device,
                                                           std::uint32_t graphics_queue_family,
                                                           bool enable_swapchain) {
    constexpr float queue_priority = 1.0F;
    VkDeviceQueueCreateInfo queue_info{};
    queue_info.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queue_info.queueFamilyIndex = graphics_queue_family;
    queue_info.queueCount = 1;
    queue_info.pQueuePriorities = &queue_priority;

    VkPhysicalDeviceVulkan13Features features_13{};
    features_13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
    features_13.dynamicRendering = VK_TRUE;
    VkPhysicalDeviceFeatures2 features{};
    features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    features.pNext = &features_13;

    VkDeviceCreateInfo device_info{};
    device_info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    device_info.pNext = &features;
    device_info.queueCreateInfoCount = 1;
    device_info.pQueueCreateInfos = &queue_info;
    const std::array<const char*, 1> swapchain_extensions{VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    if (enable_swapchain) {
        device_info.enabledExtensionCount = static_cast<std::uint32_t>(swapchain_extensions.size());
        device_info.ppEnabledExtensionNames = swapchain_extensions.data();
    }

    VkDevice device = VK_NULL_HANDLE;
    const auto result = vkCreateDevice(physical_device, &device_info, nullptr, &device);
    if (result != VK_SUCCESS) {
        return core::Result<VkDevice>::failure("renderer.vulkan_device_failed",
                                               "failed to create Vulkan logical device: " +
                                                   std::string(vk_result_name(result)));
    }
    return core::Result<VkDevice>::success(device);
}

[[nodiscard]] core::Result<VkCommandPool> create_command_pool(VkDevice device,
                                                              std::uint32_t queue_family) {
    VkCommandPoolCreateInfo pool_info{};
    pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pool_info.queueFamilyIndex = queue_family;

    VkCommandPool command_pool = VK_NULL_HANDLE;
    const auto result = vkCreateCommandPool(device, &pool_info, nullptr, &command_pool);
    if (result != VK_SUCCESS) {
        return core::Result<VkCommandPool>::failure("renderer.vulkan_command_pool_failed",
                                                    "failed to create Vulkan command pool: " +
                                                        std::string(vk_result_name(result)));
    }
    return core::Result<VkCommandPool>::success(command_pool);
}

[[nodiscard]] core::Result<VkCommandBuffer> allocate_command_buffer(VkDevice device,
                                                                    VkCommandPool command_pool) {
    VkCommandBufferAllocateInfo allocation_info{};
    allocation_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocation_info.commandPool = command_pool;
    allocation_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocation_info.commandBufferCount = 1;

    VkCommandBuffer command_buffer = VK_NULL_HANDLE;
    const auto result = vkAllocateCommandBuffers(device, &allocation_info, &command_buffer);
    if (result != VK_SUCCESS) {
        return core::Result<VkCommandBuffer>::failure("renderer.vulkan_command_buffer_failed",
                                                      "failed to allocate Vulkan command buffer: " +
                                                          std::string(vk_result_name(result)));
    }
    return core::Result<VkCommandBuffer>::success(command_buffer);
}

[[nodiscard]] core::Result<VkFence> create_fence(VkDevice device) {
    VkFenceCreateInfo fence_info{};
    fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;

    VkFence fence = VK_NULL_HANDLE;
    const auto result = vkCreateFence(device, &fence_info, nullptr, &fence);
    if (result != VK_SUCCESS) {
        return core::Result<VkFence>::failure("renderer.vulkan_fence_failed",
                                              "failed to create Vulkan fence: " +
                                                  std::string(vk_result_name(result)));
    }
    return core::Result<VkFence>::success(fence);
}

[[nodiscard]] core::Result<VkSemaphore> create_semaphore(VkDevice device) {
    VkSemaphoreCreateInfo semaphore_info{};
    semaphore_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

    VkSemaphore semaphore = VK_NULL_HANDLE;
    const auto result = vkCreateSemaphore(device, &semaphore_info, nullptr, &semaphore);
    if (result != VK_SUCCESS) {
        return core::Result<VkSemaphore>::failure("renderer.vulkan_semaphore_failed",
                                                  "failed to create Vulkan semaphore: " +
                                                      std::string(vk_result_name(result)));
    }
    return core::Result<VkSemaphore>::success(semaphore);
}

[[nodiscard]] core::Result<std::uint32_t>
find_required_memory_type(VkPhysicalDevice physical_device, std::uint32_t type_bits,
                          VkMemoryPropertyFlags required_properties, std::string_view label) {
    VkPhysicalDeviceMemoryProperties memory_properties{};
    vkGetPhysicalDeviceMemoryProperties(physical_device, &memory_properties);

    for (std::uint32_t index = 0; index < memory_properties.memoryTypeCount; ++index) {
        const auto type_supported = (type_bits & (1u << index)) != 0;
        const auto has_properties = (memory_properties.memoryTypes[index].propertyFlags &
                                     required_properties) == required_properties;
        if (type_supported && has_properties) {
            return core::Result<std::uint32_t>::success(index);
        }
    }

    return core::Result<std::uint32_t>::failure(
        "renderer.vulkan_memory_type_unavailable",
        "no compatible Vulkan memory type is available for " + std::string(label));
}

[[nodiscard]] VkBufferUsageFlags vulkan_buffer_usage_flags(rhi::RenderBufferUsage usage) noexcept {
    switch (usage) {
    case rhi::RenderBufferUsage::vertex:
        return VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    case rhi::RenderBufferUsage::index:
        return VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
    case rhi::RenderBufferUsage::uniform:
        return VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
    case rhi::RenderBufferUsage::storage:
        return VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    }
    return VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
}

[[nodiscard]] VkFilter vulkan_sampler_filter(rhi::RenderSamplerFilter value) noexcept {
    return value == rhi::RenderSamplerFilter::linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
}

[[nodiscard]] VkSamplerMipmapMode
vulkan_sampler_mipmap_mode(rhi::RenderSamplerMipmapMode value) noexcept {
    return value == rhi::RenderSamplerMipmapMode::linear ? VK_SAMPLER_MIPMAP_MODE_LINEAR
                                                         : VK_SAMPLER_MIPMAP_MODE_NEAREST;
}

[[nodiscard]] VkSamplerAddressMode
vulkan_sampler_address_mode(rhi::RenderSamplerAddressMode value) noexcept {
    return value == rhi::RenderSamplerAddressMode::clamp_to_edge
               ? VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE
               : VK_SAMPLER_ADDRESS_MODE_REPEAT;
}

[[nodiscard]] VkShaderStageFlags
vulkan_shader_stage_flags(rhi::RenderShaderStageFlags stages) noexcept {
    VkShaderStageFlags result = 0;
    if (rhi::any(stages & rhi::RenderShaderStageFlags::vertex)) {
        result |= VK_SHADER_STAGE_VERTEX_BIT;
    }
    if (rhi::any(stages & rhi::RenderShaderStageFlags::fragment)) {
        result |= VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    if (rhi::any(stages & rhi::RenderShaderStageFlags::compute)) {
        result |= VK_SHADER_STAGE_COMPUTE_BIT;
    }
    return result;
}

[[nodiscard]] VkPrimitiveTopology
vulkan_primitive_topology(rhi::RenderPrimitiveTopology topology) noexcept {
    switch (topology) {
    case rhi::RenderPrimitiveTopology::triangle_list:
        return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    case rhi::RenderPrimitiveTopology::line_list:
        return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
    }
    return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
}

[[nodiscard]] VkPolygonMode vulkan_polygon_mode(rhi::RenderPolygonMode mode) noexcept {
    switch (mode) {
    case rhi::RenderPolygonMode::fill:
        return VK_POLYGON_MODE_FILL;
    case rhi::RenderPolygonMode::line:
        return VK_POLYGON_MODE_LINE;
    }
    return VK_POLYGON_MODE_FILL;
}

[[nodiscard]] VkCullModeFlags vulkan_cull_mode(rhi::RenderCullMode mode) noexcept {
    switch (mode) {
    case rhi::RenderCullMode::none:
        return VK_CULL_MODE_NONE;
    case rhi::RenderCullMode::front:
        return VK_CULL_MODE_FRONT_BIT;
    case rhi::RenderCullMode::back:
        return VK_CULL_MODE_BACK_BIT;
    }
    return VK_CULL_MODE_NONE;
}

[[nodiscard]] VkFrontFace vulkan_front_face(rhi::RenderFrontFace face) noexcept {
    switch (face) {
    case rhi::RenderFrontFace::clockwise:
        return VK_FRONT_FACE_CLOCKWISE;
    case rhi::RenderFrontFace::counter_clockwise:
        return VK_FRONT_FACE_COUNTER_CLOCKWISE;
    }
    return VK_FRONT_FACE_COUNTER_CLOCKWISE;
}

[[nodiscard]] VkCompareOp vulkan_compare_operation(rhi::RenderCompareOperation operation) noexcept {
    switch (operation) {
    case rhi::RenderCompareOperation::never:
        return VK_COMPARE_OP_NEVER;
    case rhi::RenderCompareOperation::less:
        return VK_COMPARE_OP_LESS;
    case rhi::RenderCompareOperation::less_or_equal:
        return VK_COMPARE_OP_LESS_OR_EQUAL;
    case rhi::RenderCompareOperation::equal:
        return VK_COMPARE_OP_EQUAL;
    case rhi::RenderCompareOperation::greater:
        return VK_COMPARE_OP_GREATER;
    case rhi::RenderCompareOperation::always:
        return VK_COMPARE_OP_ALWAYS;
    }
    return VK_COMPARE_OP_LESS;
}

[[nodiscard]] bool vulkan_format_has_stencil(VkFormat format) noexcept {
    return format == VK_FORMAT_D32_SFLOAT_S8_UINT || format == VK_FORMAT_D24_UNORM_S8_UINT;
}

[[nodiscard]] VkDescriptorType vulkan_descriptor_type(rhi::RenderDescriptorKind kind) noexcept {
    switch (kind) {
    case rhi::RenderDescriptorKind::sampled_texture:
        return VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    case rhi::RenderDescriptorKind::storage_buffer:
        return VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    case rhi::RenderDescriptorKind::uniform_scalar:
    case rhi::RenderDescriptorKind::uniform_color:
        return VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    }
    return VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
}

[[nodiscard]] std::vector<VkDescriptorPoolSize>
make_descriptor_pool_sizes(const rhi::RenderPipelineLayoutDesc& desc) {
    std::uint32_t sampled_textures = 0;
    std::uint32_t uniform_buffers = 0;
    std::uint32_t storage_buffers = 0;
    for (const auto& binding : desc.descriptors) {
        switch (binding.kind) {
        case rhi::RenderDescriptorKind::sampled_texture:
            ++sampled_textures;
            break;
        case rhi::RenderDescriptorKind::storage_buffer:
            ++storage_buffers;
            break;
        case rhi::RenderDescriptorKind::uniform_scalar:
        case rhi::RenderDescriptorKind::uniform_color:
            ++uniform_buffers;
            break;
        }
    }

    std::vector<VkDescriptorPoolSize> sizes;
    sizes.reserve(3);
    if (sampled_textures > 0) {
        sizes.push_back(VkDescriptorPoolSize{
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            sampled_textures,
        });
    }
    if (uniform_buffers > 0) {
        sizes.push_back(VkDescriptorPoolSize{
            VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
            uniform_buffers,
        });
    }
    if (storage_buffers > 0) {
        sizes.push_back(VkDescriptorPoolSize{
            VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            storage_buffers,
        });
    }
    return sizes;
}

[[nodiscard]] const rhi::RenderDescriptorBinding*
find_descriptor_binding(const rhi::RenderPipelineLayoutDesc& layout,
                        std::string_view binding_name) {
    const auto found = std::ranges::find_if(
        layout.descriptors, [binding_name](const rhi::RenderDescriptorBinding& binding) {
            return binding.name == binding_name;
        });
    return found == layout.descriptors.end() ? nullptr : &*found;
}

[[nodiscard]] bool same_extent(rhi::RenderExtent left, rhi::RenderExtent right) noexcept {
    return left.width == right.width && left.height == right.height;
}

[[nodiscard]] core::Status validate_vulkan_clear_frame_plan(const rhi::RenderFramePlan& plan) {
    const auto supported_pass_count = plan.has_present_pass() ? 2U : 1U;
    if (plan.resources.size() != 1 || plan.passes.size() != supported_pass_count) {
        return core::Status::failure(
            "renderer.vulkan_unsupported_frame_plan",
            "Vulkan execute_frame_plan supports one clear target and an optional present pass");
    }
    const auto& target = plan.resources.front();
    const auto& clear = plan.passes.front();
    const auto clear_supported = target.lifetime == rhi::RenderResourceLifetime::external &&
                                 target.format == rhi::RenderImageFormat::rgba8_unorm &&
                                 same_extent(target.extent, plan.extent) &&
                                 clear.kind == rhi::RenderPassKind::clear && clear.reads.empty() &&
                                 clear.writes.size() == 1 && clear.writes.front() == target.name &&
                                 !clear.presents;
    if (!clear_supported) {
        return core::Status::failure(
            "renderer.vulkan_unsupported_frame_plan",
            "Vulkan execute_frame_plan received a clear target it cannot execute faithfully");
    }
    if (plan.passes.size() == 2) {
        const auto& present = plan.passes[1];
        const auto present_supported =
            present.kind == rhi::RenderPassKind::present && present.reads.size() == 1 &&
            present.reads.front() == target.name && present.writes.empty() && present.presents;
        if (!present_supported) {
            return core::Status::failure(
                "renderer.vulkan_unsupported_frame_plan",
                "Vulkan execute_frame_plan received a present pass it cannot execute faithfully");
        }
    }
    return core::Status::ok();
}

// The backend executes whatever graph the plan describes, so this checks only what the recorder
// genuinely cannot honour rather than pinning one hardcoded pass layout. It used to require an
// exact eight-pass colour/depth frame by name, which is why no other graph could run at all.
[[nodiscard]] core::Status
validate_vulkan_frame_plan_is_executable(const rhi::RenderFrameSubmission& frame) {
    const auto& plan = frame.plan;
    const auto* depth = plan.find_resource(rhi::depth_resource_name);
    if (depth == nullptr || !rhi::is_depth_format(depth->format)) {
        return core::Status::failure(
            "renderer.vulkan_unsupported_frame_plan",
            "Vulkan execute_frame needs the graph to declare a depth resource");
    }
    for (const auto& resource : plan.resources) {
        if (!same_extent(resource.extent, plan.extent)) {
            return core::Status::failure("renderer.vulkan_unsupported_frame_plan",
                                         "frame graph resource '" + resource.name +
                                             "' does not match the frame extent");
        }
    }
    for (const auto& pass : plan.passes) {
        if (pass.kind == rhi::RenderPassKind::present) {
            continue;
        }
        // One colour attachment per pass: multiple render targets are not wired through the
        // recorder yet, and silently dropping the extras would be worse than refusing.
        const auto colour_writes = std::ranges::count_if(
            pass.writes, [](const std::string& written) {
                return written != rhi::depth_resource_name;
            });
        if (colour_writes > 1) {
            return core::Status::failure(
                "renderer.vulkan_unsupported_frame_plan",
                "frame graph pass '" + pass.name +
                    "' writes more than one colour attachment, which the Vulkan recorder does not "
                    "support yet");
        }
    }
    const auto pass_count = plan.passes.size();
    const auto command_passes_supported = std::ranges::all_of(
        frame.pass_commands, [pass_count](const rhi::RenderPassCommands& commands) {
            return commands.pass_index < pass_count;
        });
    if (!command_passes_supported) {
        return core::Status::failure("renderer.vulkan_unsupported_frame_plan",
                                     "frame submission records draws for a pass the plan does not "
                                     "declare");
    }
    return core::Status::ok();
}

struct VulkanResourceStateSync {
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkAccessFlags access = 0;
    VkPipelineStageFlags stage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
};

struct VulkanFrameTransition {
    std::string resource_name;
    VkImageLayout old_layout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImageLayout new_layout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkAccessFlags source_access = 0;
    VkAccessFlags destination_access = 0;
    VkPipelineStageFlags source_stage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    VkPipelineStageFlags destination_stage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    bool has_source_use = false;
    std::size_t source_pass_index = 0;
    std::size_t destination_pass_index = 0;
};

[[nodiscard]] VulkanResourceStateSync
vulkan_sync_for_resource_state(rhi::RenderResourceState state) noexcept {
    switch (state) {
    case rhi::RenderResourceState::undefined:
        return VulkanResourceStateSync{
            VK_IMAGE_LAYOUT_UNDEFINED,
            0,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
        };
    case rhi::RenderResourceState::external:
        return VulkanResourceStateSync{
            VK_IMAGE_LAYOUT_GENERAL,
            VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
            VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
        };
    case rhi::RenderResourceState::shader_read:
        return VulkanResourceStateSync{
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_ACCESS_SHADER_READ_BIT,
            VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        };
    case rhi::RenderResourceState::color_attachment_write:
        return VulkanResourceStateSync{
            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        };
    case rhi::RenderResourceState::color_attachment_read_write:
        return VulkanResourceStateSync{
            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        };
    case rhi::RenderResourceState::depth_attachment_write:
        return VulkanResourceStateSync{
            VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
            VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
            VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
        };
    case rhi::RenderResourceState::transfer_source:
        return VulkanResourceStateSync{
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VK_ACCESS_TRANSFER_READ_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT,
        };
    case rhi::RenderResourceState::transfer_destination:
        return VulkanResourceStateSync{
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT,
        };
    case rhi::RenderResourceState::present:
        return VulkanResourceStateSync{
            VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
            0,
            VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
        };
    }
    return VulkanResourceStateSync{};
}

[[nodiscard]] core::Result<std::vector<VulkanFrameTransition>>
translate_frame_transitions_for_vulkan(const rhi::RenderFrameExecutionPlan& execution_plan) {
    std::vector<VulkanFrameTransition> translated;
    translated.reserve(execution_plan.transitions.size());

    for (const auto& transition : execution_plan.transitions) {
        if (transition.destination_use_index >= execution_plan.resource_uses.size()) {
            return core::Result<std::vector<VulkanFrameTransition>>::failure(
                "renderer.vulkan_invalid_frame_transition",
                "frame transition destination use index is out of range");
        }
        if (transition.has_source_use &&
            transition.source_use_index >= execution_plan.resource_uses.size()) {
            return core::Result<std::vector<VulkanFrameTransition>>::failure(
                "renderer.vulkan_invalid_frame_transition",
                "frame transition source use index is out of range");
        }

        const auto source = vulkan_sync_for_resource_state(transition.before_state);
        const auto destination = vulkan_sync_for_resource_state(transition.after_state);
        translated.push_back(VulkanFrameTransition{
            transition.resource_name,
            source.layout,
            destination.layout,
            source.access,
            destination.access,
            source.stage,
            destination.stage,
            transition.has_source_use,
            transition.source_pass_index,
            transition.destination_pass_index,
        });
    }

    return core::Result<std::vector<VulkanFrameTransition>>::success(std::move(translated));
}

[[nodiscard]] std::string
present_target_resource_name(const rhi::RenderFrameExecutionPlan& execution_plan) {
    const auto transition = std::ranges::find_if(
        execution_plan.transitions, [](const rhi::RenderFrameResourceTransition& candidate) {
            return candidate.after_state == rhi::RenderResourceState::present;
        });
    if (transition != execution_plan.transitions.end()) {
        return transition->resource_name;
    }

    const auto use = std::ranges::find_if(
        execution_plan.resource_uses, [](const rhi::RenderFrameResourceUse& candidate) {
            return candidate.access == rhi::RenderResourceAccess::present;
        });
    return use == execution_plan.resource_uses.end() ? std::string{} : use->resource_name;
}

[[nodiscard]] std::string
offscreen_target_resource_name(const rhi::RenderFrameExecutionPlan& execution_plan) {
    const auto written_use = std::ranges::find_if(
        execution_plan.resource_uses, [](const rhi::RenderFrameResourceUse& candidate) {
            return candidate.access == rhi::RenderResourceAccess::write ||
                   candidate.access == rhi::RenderResourceAccess::read_write;
        });
    if (written_use != execution_plan.resource_uses.end()) {
        return written_use->resource_name;
    }
    return execution_plan.resource_uses.empty()
               ? std::string{}
               : execution_plan.resource_uses.front().resource_name;
}

[[nodiscard]] std::vector<VulkanFrameTransition>
transitions_for_resource(std::span<const VulkanFrameTransition> transitions,
                         std::string_view resource_name) {
    std::vector<VulkanFrameTransition> filtered;
    filtered.reserve(transitions.size());
    for (const auto& transition : transitions) {
        if (transition.resource_name == resource_name) {
            filtered.push_back(transition);
        }
    }
    return filtered;
}

[[nodiscard]] std::vector<std::string>
transition_resource_names(std::span<const VulkanFrameTransition> transitions) {
    std::vector<std::string> names;
    names.reserve(transitions.size());
    for (const auto& transition : transitions) {
        if (std::ranges::none_of(names, [&transition](const std::string& name) {
                return name == transition.resource_name;
            })) {
            names.push_back(transition.resource_name);
        }
    }
    return names;
}

std::size_t record_planned_image_transition_barriers(
    VkCommandBuffer command_buffer, VkImage image, const VkImageSubresourceRange& range,
    VkImageLayout& current_layout, std::span<const VulkanFrameTransition> transitions) {
    std::size_t submitted_barriers = 0;
    for (const auto& transition : transitions) {
        VkImageMemoryBarrier barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.srcAccessMask =
            current_layout == VK_IMAGE_LAYOUT_UNDEFINED ? 0 : transition.source_access;
        barrier.dstAccessMask = transition.destination_access;
        barrier.oldLayout = current_layout;
        barrier.newLayout = transition.new_layout;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = image;
        barrier.subresourceRange = range;

        VkPipelineStageFlags source_stage = transition.source_stage;
        if (current_layout == VK_IMAGE_LAYOUT_UNDEFINED) {
            source_stage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        }
        vkCmdPipelineBarrier(command_buffer, source_stage, transition.destination_stage, 0, 0,
                             nullptr, 0, nullptr, 1, &barrier);
        current_layout = transition.new_layout;
        ++submitted_barriers;
    }
    return submitted_barriers;
}

class VulkanSmokeDevice final : public rhi::IRenderDevice {
  private:
    struct VulkanBufferResource {
        rhi::RenderBufferDesc desc;
        VkBuffer buffer = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        std::size_t byte_size = 0;
    };

    struct VulkanShaderModuleResource {
        rhi::RenderShaderModuleDesc desc;
        VkShaderModule shader_module = VK_NULL_HANDLE;
        std::size_t word_count = 0;
    };

    struct VulkanImageResource {
        rhi::RenderImageDesc desc;
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkImageView image_view = VK_NULL_HANDLE;
        VkSampler sampler = VK_NULL_HANDLE;
        bool owns_sampler = false;
        VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
        std::size_t byte_size = 0;
    };

    struct VulkanSamplerResource {
        rhi::RenderSamplerDesc desc;
        VkSampler sampler = VK_NULL_HANDLE;
    };

    struct VulkanPipelineLayoutResource {
        rhi::RenderPipelineLayoutDesc desc;
        VkDescriptorSetLayout descriptor_set_layout = VK_NULL_HANDLE;
        VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
        VkDescriptorPool descriptor_pool = VK_NULL_HANDLE;
        VkDescriptorSet descriptor_set = VK_NULL_HANDLE;
        // Populated only when the layout declares per_frame_descriptors. Indexed by frame context,
        // so updating this frame's set cannot touch one a previous frame is still reading.
        std::vector<VkDescriptorSet> per_frame_descriptor_sets;
        std::uint64_t last_used_submission_serial = 0;
    };

    struct VulkanComputePipelineResource {
        rhi::RenderComputePipelineDesc desc;
        VkPipeline pipeline = VK_NULL_HANDLE;
    };

    struct VulkanGraphicsPipelineResource {
        rhi::RenderGraphicsPipelineDesc desc;
        VkPipeline pipeline = VK_NULL_HANDLE;
        // Attachment formats the pipeline was created against. Kept so a frame can assert that the
        // images it binds match what the pipeline expects.
        VkFormat color_format = VK_FORMAT_UNDEFINED;
        VkFormat depth_format = VK_FORMAT_UNDEFINED;
        bool uses_depth = false;
    };

    struct VulkanFrameImageResource {
        std::string name;
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
    };

    static constexpr std::uint32_t timestamp_slot_count = 3;
    static constexpr std::uint32_t timestamps_per_slot = 12;

    enum TimestampIndex : std::uint32_t {
        frame_start_timestamp = 0,
        opaque_start_timestamp = 1,
        opaque_end_timestamp = 2,
        alpha_tested_start_timestamp = 3,
        alpha_tested_end_timestamp = 4,
        transparent_start_timestamp = 5,
        transparent_end_timestamp = 6,
        transfer_start_timestamp = 7,
        final_copy_start_timestamp = 8,
        final_copy_end_timestamp = 9,
        transfer_end_timestamp = 10,
        frame_end_timestamp = 11,
    };

    struct VulkanGpuTimingSample {
        bool valid = false;
        std::uint64_t frame_index = 0;
        double frame_ms = 0.0;
        double opaque_ms = 0.0;
        double alpha_tested_ms = 0.0;
        double transparent_ms = 0.0;
        double transfer_ms = 0.0;
        double final_copy_ms = 0.0;
    };

    struct VulkanFrameTarget {
        VkImage color_image = VK_NULL_HANDLE;
        VkImageView color_view = VK_NULL_HANDLE;
        VkDeviceMemory color_memory = VK_NULL_HANDLE;
        VkImageLayout color_layout = VK_IMAGE_LAYOUT_UNDEFINED;
        VkImage depth_image = VK_NULL_HANDLE;
        VkImageView depth_view = VK_NULL_HANDLE;
        VkDeviceMemory depth_memory = VK_NULL_HANDLE;
        VkImageLayout depth_layout = VK_IMAGE_LAYOUT_UNDEFINED;
        rhi::RenderExtent extent{};
    };

    struct VulkanFrameContext {
        VkCommandPool command_pool = VK_NULL_HANDLE;
        VkCommandBuffer graphics_commands = VK_NULL_HANDLE;
        VkFence completion_fence = VK_NULL_HANDLE;
        VkSemaphore image_available = VK_NULL_HANDLE;
        VulkanFrameTarget target;
        // Backs the resources the frame graph declares. Per frame context, because a frame in
        // flight must not have its attachments overwritten by the next one.
        VulkanFrameResourcePool resources;
        std::uint64_t submission_serial = 0;
        bool in_flight = false;
    };

    struct VulkanUploadContext {
        VkCommandPool command_pool = VK_NULL_HANDLE;
        VkCommandBuffer transfer_commands = VK_NULL_HANDLE;
        VkFence completion_fence = VK_NULL_HANDLE;
        VkQueryPool timestamp_queries = VK_NULL_HANDLE;
        VulkanBufferResource fallback_staging;
        void* fallback_mapped = nullptr;
        std::uint64_t submission_serial = 0;
        bool in_flight = false;
        bool timing_pending = false;
    };

    template <typename Resource> struct RetiredVulkanResource {
        Resource resource;
        std::uint64_t retire_after_submission = 0;
    };

  public:
    VulkanSmokeDevice(rhi::RenderDeviceDesc desc, VulkanInstanceResource instance,
                      SelectedPhysicalDevice selected, VkDevice device, VkQueue queue,
                      VkSurfaceKHR surface, VkCommandPool command_pool,
                      VkCommandBuffer command_buffer, VkFence fence,
                      VkSemaphore image_available_semaphore, VkSemaphore render_finished_semaphore)
        : desc_(std::move(desc)), instance_(instance.instance),
          debug_messenger_(instance.debug_messenger),
          validation_enabled_(instance.validation_enabled),
          debug_utils_enabled_(instance.debug_utils_enabled),
          physical_device_(selected.physical_device), properties_(selected.properties),
          graphics_queue_family_(selected.graphics_queue_family),
          depth_format_(selected.depth_format), device_(device), queue_(queue),
          timestamp_valid_bits_(selected.timestamp_valid_bits), surface_(surface),
          command_pool_(command_pool), command_buffer_(command_buffer), fence_(fence),
          image_available_semaphore_(image_available_semaphore),
          render_finished_semaphore_(render_finished_semaphore) {
        initialize_instrumentation();
    }

    ~VulkanSmokeDevice() override {
        if (device_ != VK_NULL_HANDLE) {
            vkDeviceWaitIdle(device_);
            complete_all_submissions();
            destroy_frame_contexts();
            destroy_staging_buffer();
            destroy_buffer_resources();
            destroy_image_resources();
            destroy_sampler_resources();
            if (default_sampler_ != VK_NULL_HANDLE) {
                vkDestroySampler(device_, default_sampler_, nullptr);
                default_sampler_ = VK_NULL_HANDLE;
            }
            destroy_graphics_pipeline_resources();
            destroy_compute_pipeline_resources();
            destroy_shader_module_resources();
            destroy_pipeline_layout_resources();
            destroy_swapchain();
            destroy_offscreen_target();
            if (timestamp_query_pool_ != VK_NULL_HANDLE) {
                vkDestroyQueryPool(device_, timestamp_query_pool_, nullptr);
                timestamp_query_pool_ = VK_NULL_HANDLE;
            }
            if (render_finished_semaphore_ != VK_NULL_HANDLE) {
                vkDestroySemaphore(device_, render_finished_semaphore_, nullptr);
            }
            if (image_available_semaphore_ != VK_NULL_HANDLE) {
                vkDestroySemaphore(device_, image_available_semaphore_, nullptr);
            }
            if (fence_ != VK_NULL_HANDLE) {
                vkDestroyFence(device_, fence_, nullptr);
            }
            if (command_pool_ != VK_NULL_HANDLE) {
                vkDestroyCommandPool(device_, command_pool_, nullptr);
            }
            vkDestroyDevice(device_, nullptr);
        }
        if (surface_ != VK_NULL_HANDLE) {
            vkDestroySurfaceKHR(instance_, surface_, nullptr);
        }
        if (instance_ != VK_NULL_HANDLE) {
            if (debug_messenger_ != VK_NULL_HANDLE) {
                const auto destroy_debug_messenger =
                    reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
                        vkGetInstanceProcAddr(instance_, "vkDestroyDebugUtilsMessengerEXT"));
                if (destroy_debug_messenger != nullptr) {
                    destroy_debug_messenger(instance_, debug_messenger_, nullptr);
                }
                debug_messenger_ = VK_NULL_HANDLE;
            }
            vkDestroyInstance(instance_, nullptr);
        }
    }

    VulkanSmokeDevice(const VulkanSmokeDevice&) = delete;
    VulkanSmokeDevice& operator=(const VulkanSmokeDevice&) = delete;
    VulkanSmokeDevice(VulkanSmokeDevice&&) = delete;
    VulkanSmokeDevice& operator=(VulkanSmokeDevice&&) = delete;

    [[nodiscard]] core::Status initialize_frame_contexts() {
        frame_contexts_.reserve(desc_.frames_in_flight);
        upload_contexts_.reserve(desc_.frames_in_flight);
        for (std::uint32_t index = 0; index < desc_.frames_in_flight; ++index) {
            auto frame = create_frame_context();
            if (!frame) {
                return core::Status::failure(frame.error().code, frame.error().message);
            }
            frame_contexts_.push_back(std::move(frame).value());

            auto upload = create_upload_context();
            if (!upload) {
                return core::Status::failure(upload.error().code, upload.error().message);
            }
            upload_contexts_.push_back(std::move(upload).value());
        }
        return core::Status::ok();
    }

    [[nodiscard]] rhi::RenderBackend backend() const noexcept override {
        return rhi::RenderBackend::vulkan;
    }

    [[nodiscard]] std::string_view backend_name() const noexcept override {
        return rhi::render_backend_name(rhi::RenderBackend::vulkan);
    }

    [[nodiscard]] rhi::RenderDeviceCapabilities capabilities() const noexcept override {
        const auto max_dimension =
            std::max<std::uint32_t>(1, properties_.limits.maxImageDimension2D);
        rhi::RenderDeviceCapabilities result;
        result.backend = rhi::RenderBackend::vulkan;
        result.max_extent = rhi::RenderExtent{max_dimension, max_dimension};
        result.supports_present = surface_ != VK_NULL_HANDLE;
        result.supports_validation = validation_enabled_;
        result.supports_debug_markers = debug_utils_enabled_;
        result.supports_shader_modules = true;
        result.supports_pipeline_layout = true;
        result.supports_compute_pipelines = true;
        result.supports_graphics_pipelines = true;
        result.supports_descriptor_writes = true;
        result.supports_buffer_upload = true;
        result.supports_image_upload = true;
        result.supports_sampler_cache = true;
        result.supports_draw_binding = true;
        result.supports_frame_submission = true;
        result.supports_depth = depth_format_ != VK_FORMAT_UNDEFINED;
        result.headless = false;
        return result;
    }

    [[nodiscard]] rhi::RenderExtent current_extent() const noexcept override {
        if (swapchain_ != VK_NULL_HANDLE) {
            return swapchain_extent_;
        }
        return desc_.initial_extent;
    }

    [[nodiscard]] std::uint64_t completed_frame_count() const noexcept override {
        return completed_frame_count_;
    }

    [[nodiscard]] std::uint64_t last_submission_serial() const noexcept override {
        return last_submission_serial_;
    }

    [[nodiscard]] std::uint64_t completed_submission_serial() const noexcept override {
        return completed_submission_serial_;
    }

    [[nodiscard]] std::size_t live_resource_count() const noexcept override {
        return buffer_resources_.size() + image_resources_.size() + sampler_resources_.size() +
               shader_modules_.size() + compute_pipelines_.size() + graphics_pipelines_.size() +
               retired_buffers_.size() + retired_images_.size() + retired_samplers_.size() +
               retired_shader_modules_.size() + retired_compute_pipelines_.size() +
               retired_graphics_pipelines_.size();
    }

    [[nodiscard]] core::Status wait_idle() override {
        const auto idle_result = vkDeviceWaitIdle(device_);
        if (idle_result != VK_SUCCESS) {
            return core::Status::failure("renderer.vulkan_wait_idle_failed",
                                         "failed to idle Vulkan device: " +
                                             std::string(vk_result_name(idle_result)));
        }
        complete_all_submissions();
        return core::Status::ok();
    }

    // Copies the image the last frame resolved into back to the CPU. Idles the device and uses a
    // throwaway staging buffer, so this is a test and tooling facility, not a frame operation.
    [[nodiscard]] core::Result<std::vector<std::uint8_t>> read_back_output_image() override {
        using Result = core::Result<std::vector<std::uint8_t>>;
        if (frame_contexts_.empty()) {
            return Result::failure("renderer.readback_unavailable",
                                   "no frame context has rendered an image to read back");
        }
        auto idle = wait_idle();
        if (!idle) {
            return Result::failure(idle.error().code, idle.error().message);
        }
        // next_frame_context_ has already advanced past the frame that was just submitted.
        const auto completed =
            (next_frame_context_ + frame_contexts_.size() - 1) % frame_contexts_.size();
        const auto& target = frame_contexts_[completed].target;
        if (target.color_image == VK_NULL_HANDLE || !target.extent.is_valid()) {
            return Result::failure("renderer.readback_unavailable",
                                   "the last frame context holds no colour image");
        }
        const auto extent = target.extent;
        const auto byte_size =
            static_cast<VkDeviceSize>(extent.width) * extent.height * 4U;

        VulkanBufferResource staging;
        staging.byte_size = static_cast<std::size_t>(byte_size);
        VkBufferCreateInfo buffer_info{};
        buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        buffer_info.size = byte_size;
        buffer_info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        auto result = vkCreateBuffer(device_, &buffer_info, nullptr, &staging.buffer);
        if (result != VK_SUCCESS) {
            return Result::failure("renderer.vulkan_buffer_failed",
                                   "failed to create readback buffer: " +
                                       std::string(vk_result_name(result)));
        }
        VkMemoryRequirements requirements{};
        vkGetBufferMemoryRequirements(device_, staging.buffer, &requirements);
        auto memory_type = find_required_memory_type(
            physical_device_, requirements.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
            "image readback");
        if (!memory_type) {
            destroy_buffer_resource(staging);
            return Result::failure(memory_type.error().code, memory_type.error().message);
        }
        VkMemoryAllocateInfo allocation_info{};
        allocation_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocation_info.allocationSize = requirements.size;
        allocation_info.memoryTypeIndex = memory_type.value();
        result = vkAllocateMemory(device_, &allocation_info, nullptr, &staging.memory);
        if (result == VK_SUCCESS) {
            result = vkBindBufferMemory(device_, staging.buffer, staging.memory, 0);
        }
        if (result != VK_SUCCESS) {
            destroy_buffer_resource(staging);
            return Result::failure("renderer.vulkan_buffer_memory_failed",
                                   "failed to back readback buffer: " +
                                       std::string(vk_result_name(result)));
        }

        const auto record = [&]() -> core::Status {
            auto status = vkResetFences(device_, 1, &fence_);
            if (status == VK_SUCCESS) {
                status = vkResetCommandBuffer(command_buffer_, 0);
            }
            if (status != VK_SUCCESS) {
                return core::Status::failure("renderer.vulkan_reset_failed",
                                             "failed to reset readback command state: " +
                                                 std::string(vk_result_name(status)));
            }
            VkCommandBufferBeginInfo begin_info{};
            begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
            begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            status = vkBeginCommandBuffer(command_buffer_, &begin_info);
            if (status != VK_SUCCESS) {
                return core::Status::failure("renderer.vulkan_begin_command_buffer_failed",
                                             "failed to begin readback command buffer: " +
                                                 std::string(vk_result_name(status)));
            }
            // The frame leaves the image in transfer-source layout after its blit to the
            // swapchain, but an offscreen frame may not have transitioned it at all.
            VkImageMemoryBarrier to_source{};
            to_source.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            to_source.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
            to_source.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            to_source.oldLayout = target.color_layout;
            to_source.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            to_source.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            to_source.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            to_source.image = target.color_image;
            to_source.subresourceRange =
                VkImageSubresourceRange{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            vkCmdPipelineBarrier(command_buffer_, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                 VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                                 &to_source);

            VkBufferImageCopy copy{};
            copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            copy.imageSubresource.layerCount = 1;
            copy.imageExtent = VkExtent3D{extent.width, extent.height, 1};
            vkCmdCopyImageToBuffer(command_buffer_, target.color_image,
                                   VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, staging.buffer, 1, &copy);

            status = vkEndCommandBuffer(command_buffer_);
            if (status != VK_SUCCESS) {
                return core::Status::failure("renderer.vulkan_end_command_buffer_failed",
                                             "failed to end readback command buffer: " +
                                                 std::string(vk_result_name(status)));
            }
            VkSubmitInfo submit_info{};
            submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
            submit_info.commandBufferCount = 1;
            submit_info.pCommandBuffers = &command_buffer_;
            status = vkQueueSubmit(queue_, 1, &submit_info, fence_);
            if (status != VK_SUCCESS) {
                return core::Status::failure("renderer.vulkan_queue_submit_failed",
                                             "failed to submit readback: " +
                                                 std::string(vk_result_name(status)));
            }
            status = vkWaitForFences(device_, 1, &fence_, VK_TRUE,
                                     std::numeric_limits<std::uint64_t>::max());
            if (status != VK_SUCCESS) {
                return core::Status::failure("renderer.vulkan_wait_fence_failed",
                                             "failed to wait for readback: " +
                                                 std::string(vk_result_name(status)));
            }
            return core::Status::ok();
        };

        const auto recorded = record();
        if (!recorded) {
            destroy_buffer_resource(staging);
            return Result::failure(recorded.error().code, recorded.error().message);
        }

        void* mapped = nullptr;
        result = vkMapMemory(device_, staging.memory, 0, byte_size, 0, &mapped);
        if (result != VK_SUCCESS) {
            destroy_buffer_resource(staging);
            return Result::failure("renderer.vulkan_map_memory_failed",
                                   "failed to map readback buffer: " +
                                       std::string(vk_result_name(result)));
        }
        std::vector<std::uint8_t> pixels(static_cast<std::size_t>(byte_size));
        std::memcpy(pixels.data(), mapped, pixels.size());
        vkUnmapMemory(device_, staging.memory);
        destroy_buffer_resource(staging);
        // The image the frame renders into is R8G8B8A8_UNORM, so this is already display-ready.
        return Result::success(std::move(pixels));
    }

    [[nodiscard]] core::Status resize(rhi::RenderExtent extent) override {
        auto status = rhi::validate_render_extent(extent);
        if (!status) {
            return status;
        }
        desc_.initial_extent = extent;
        const auto idle_result = vkDeviceWaitIdle(device_);
        if (idle_result != VK_SUCCESS) {
            return core::Status::failure("renderer.vulkan_wait_idle_failed",
                                         "failed to idle Vulkan device before resize: " +
                                             std::string(vk_result_name(idle_result)));
        }
        complete_all_submissions();
        if (swapchain_ != VK_NULL_HANDLE) {
            destroy_swapchain();
        }
        destroy_offscreen_target();
        for (auto& context : frame_contexts_) {
            destroy_frame_target(context.target);
        }
        return core::Status::ok();
    }

    [[nodiscard]] core::Result<rhi::RenderFrameStats>
    render_frame(rhi::RenderFrameDesc desc) override {
        const auto use_current_extent =
            desc.output_extent.width == 0 && desc.output_extent.height == 0;
        const auto extent = use_current_extent ? current_extent() : desc.output_extent;
        const auto plan =
            rhi::make_clear_present_frame_plan(extent, desc.clear_color, desc.present);
        return execute_frame_plan(plan);
    }

    [[nodiscard]] core::Result<rhi::RenderFrameStats>
    execute_frame_plan(const rhi::RenderFramePlan& plan) override {
        auto execution_plan = plan.build_execution_plan();
        if (!execution_plan) {
            return core::Result<rhi::RenderFrameStats>::failure(execution_plan.error().code,
                                                                execution_plan.error().message);
        }
        const auto compatibility_status = validate_vulkan_clear_frame_plan(plan);
        if (!compatibility_status) {
            return core::Result<rhi::RenderFrameStats>::failure(
                compatibility_status.error().code, compatibility_status.error().message);
        }

        const auto clear_color = plan.first_clear_color();
        if (plan.has_present_pass()) {
            if (surface_ == VK_NULL_HANDLE) {
                return core::Result<rhi::RenderFrameStats>::failure(
                    "renderer.vulkan_present_unavailable",
                    "Vulkan smoke device has no window surface for presentation");
            }
            if (desc_.initial_extent.width != plan.extent.width ||
                desc_.initial_extent.height != plan.extent.height) {
                desc_.initial_extent = plan.extent;
                if (swapchain_ != VK_NULL_HANDLE) {
                    destroy_swapchain();
                }
            }
            auto translated_transitions =
                translate_frame_transitions_for_vulkan(execution_plan.value());
            if (!translated_transitions) {
                return core::Result<rhi::RenderFrameStats>::failure(
                    translated_transitions.error().code, translated_transitions.error().message);
            }
            auto target_transitions =
                transitions_for_resource(translated_transitions.value(),
                                         present_target_resource_name(execution_plan.value()));
            auto present_stats = render_present_frame(
                rhi::RenderFrameDesc{
                    clear_color,
                    plan.extent,
                    true,
                },
                target_transitions);
            if (!present_stats) {
                return present_stats;
            }
            present_stats.value().render_pass_count = execution_plan.value().ordered_passes.size();
            present_stats.value().present_pass_count = execution_plan.value().present_pass_count;
            present_stats.value().resource_use_count = execution_plan.value().resource_uses.size();
            present_stats.value().dependency_count = execution_plan.value().dependencies.size();
            present_stats.value().transition_count = execution_plan.value().transitions.size();
            present_stats.value().synchronization_barrier_count =
                translated_transitions.value().size();
            present_stats.value().submitted_synchronization_barrier_count =
                target_transitions.size();
            present_stats.value().clear_color = clear_color;
            return present_stats;
        }

        auto translated_transitions =
            translate_frame_transitions_for_vulkan(execution_plan.value());
        if (!translated_transitions) {
            return core::Result<rhi::RenderFrameStats>::failure(
                translated_transitions.error().code, translated_transitions.error().message);
        }
        auto target_transitions = transitions_for_resource(
            translated_transitions.value(), offscreen_target_resource_name(execution_plan.value()));

        auto status = ensure_offscreen_target(plan.extent);
        if (!status) {
            return core::Result<rhi::RenderFrameStats>::failure(status.error().code,
                                                                status.error().message);
        }

        auto submitted_barriers =
            submit_offscreen_clear(clear_color, translated_transitions.value(), target_transitions);
        if (!submitted_barriers) {
            return core::Result<rhi::RenderFrameStats>::failure(submitted_barriers.error().code,
                                                                submitted_barriers.error().message);
        }

        rhi::RenderFrameStats stats;
        stats.backend = backend();
        stats.frame_index = next_frame_index_++;
        stats.submission_serial = ++last_submission_serial_;
        complete_all_submissions();
        advance_completed_submission(stats.submission_serial);
        stats.completed_submission_serial = completed_submission_serial_;
        stats.extent = execution_plan.value().extent;
        stats.clear_color = clear_color;
        stats.presented = false;
        stats.render_pass_count = execution_plan.value().ordered_passes.size();
        stats.present_pass_count = 0;
        stats.resource_use_count = execution_plan.value().resource_uses.size();
        stats.dependency_count = execution_plan.value().dependencies.size();
        stats.transition_count = execution_plan.value().transitions.size();
        stats.synchronization_barrier_count = translated_transitions.value().size();
        stats.submitted_synchronization_barrier_count = submitted_barriers.value();
        attach_latest_gpu_upload_timing(stats);
        ++completed_frame_count_;
        return core::Result<rhi::RenderFrameStats>::success(stats);
    }

    [[nodiscard]] core::Result<rhi::RenderFrameStats>
    execute_frame(const rhi::RenderFrameSubmission& frame) override {
        auto status = rhi::validate_render_frame_submission_shape(frame);
        if (!status) {
            return core::Result<rhi::RenderFrameStats>::failure(status.error().code,
                                                                status.error().message);
        }
        status = validate_vulkan_frame_plan_is_executable(frame);
        if (!status) {
            return core::Result<rhi::RenderFrameStats>::failure(status.error().code,
                                                                status.error().message);
        }
        return execute_submitted_frame(frame);
    }

    [[nodiscard]] core::Result<rhi::RenderUploadStats>
    upload_buffer(rhi::RenderBufferDesc desc, std::span<const std::byte> bytes) override {
        auto status = rhi::validate_render_buffer_upload(desc, bytes);
        if (!status) {
            return core::Result<rhi::RenderUploadStats>::failure(status.error().code,
                                                                 status.error().message);
        }

        VulkanBufferResource resource;
        resource.desc = desc;
        resource.byte_size = bytes.size();

        VkBufferCreateInfo buffer_info{};
        buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        buffer_info.size = static_cast<VkDeviceSize>(bytes.size());
        buffer_info.usage = vulkan_buffer_usage_flags(desc.usage);
        buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

        auto result = vkCreateBuffer(device_, &buffer_info, nullptr, &resource.buffer);
        if (result != VK_SUCCESS) {
            return core::Result<rhi::RenderUploadStats>::failure(
                "renderer.vulkan_buffer_failed",
                "failed to create Vulkan upload buffer: " + std::string(vk_result_name(result)));
        }

        VkMemoryRequirements requirements{};
        vkGetBufferMemoryRequirements(device_, resource.buffer, &requirements);
        auto memory_type = find_required_memory_type(physical_device_, requirements.memoryTypeBits,
                                                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                         VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                                                     "uploaded render buffer");
        if (!memory_type) {
            destroy_buffer_resource(resource);
            return core::Result<rhi::RenderUploadStats>::failure(memory_type.error().code,
                                                                 memory_type.error().message);
        }

        VkMemoryAllocateInfo allocation_info{};
        allocation_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocation_info.allocationSize = requirements.size;
        allocation_info.memoryTypeIndex = memory_type.value();

        result = vkAllocateMemory(device_, &allocation_info, nullptr, &resource.memory);
        if (result != VK_SUCCESS) {
            destroy_buffer_resource(resource);
            return core::Result<rhi::RenderUploadStats>::failure(
                "renderer.vulkan_buffer_memory_failed",
                "failed to allocate Vulkan upload buffer memory: " +
                    std::string(vk_result_name(result)));
        }

        result = vkBindBufferMemory(device_, resource.buffer, resource.memory, 0);
        if (result != VK_SUCCESS) {
            destroy_buffer_resource(resource);
            return core::Result<rhi::RenderUploadStats>::failure(
                "renderer.vulkan_buffer_bind_failed",
                "failed to bind Vulkan upload buffer memory: " +
                    std::string(vk_result_name(result)));
        }

        void* mapped_memory = nullptr;
        result = vkMapMemory(device_, resource.memory, 0, static_cast<VkDeviceSize>(bytes.size()),
                             0, &mapped_memory);
        if (result != VK_SUCCESS) {
            destroy_buffer_resource(resource);
            return core::Result<rhi::RenderUploadStats>::failure(
                "renderer.vulkan_buffer_map_failed", "failed to map Vulkan upload buffer memory: " +
                                                         std::string(vk_result_name(result)));
        }
        std::memcpy(mapped_memory, bytes.data(), bytes.size());
        vkUnmapMemory(device_, resource.memory);

        const rhi::RenderResourceHandle handle{next_resource_id_++};
        buffer_resources_.emplace(handle.value, std::move(resource));

        rhi::RenderUploadStats stats;
        stats.backend = backend();
        stats.handle = handle;
        stats.usage = desc.usage;
        stats.byte_size = bytes.size();
        stats.live_resource_count = buffer_resources_.size();
        stats.gpu_backed = true;
        return core::Result<rhi::RenderUploadStats>::success(stats);
    }

    [[nodiscard]] core::Result<rhi::RenderBufferCreateStats>
    create_buffer(rhi::RenderBufferDesc desc) override {
        auto status = rhi::validate_render_buffer_desc(desc);
        if (!status) {
            return core::Result<rhi::RenderBufferCreateStats>::failure(status.error().code,
                                                                       status.error().message);
        }

        VulkanBufferResource resource;
        resource.desc = desc;
        resource.byte_size = desc.byte_size;
        VkBufferCreateInfo buffer_info{};
        buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        buffer_info.size = static_cast<VkDeviceSize>(desc.byte_size);
        buffer_info.usage =
            vulkan_buffer_usage_flags(desc.usage) | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

        auto result = vkCreateBuffer(device_, &buffer_info, nullptr, &resource.buffer);
        if (result != VK_SUCCESS) {
            return core::Result<rhi::RenderBufferCreateStats>::failure(
                "renderer.vulkan_buffer_failed",
                "failed to create Vulkan buffer: " + std::string(vk_result_name(result)));
        }

        VkMemoryRequirements requirements{};
        vkGetBufferMemoryRequirements(device_, resource.buffer, &requirements);
        const VkMemoryPropertyFlags properties =
            desc.memory == rhi::RenderBufferMemory::device_local
                ? VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT
                : VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        auto memory_type = find_required_memory_type(physical_device_, requirements.memoryTypeBits,
                                                     properties, desc.debug_name);
        if (!memory_type) {
            destroy_buffer_resource(resource);
            return core::Result<rhi::RenderBufferCreateStats>::failure(memory_type.error().code,
                                                                       memory_type.error().message);
        }

        VkMemoryAllocateInfo allocation_info{};
        allocation_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocation_info.allocationSize = requirements.size;
        allocation_info.memoryTypeIndex = memory_type.value();
        result = vkAllocateMemory(device_, &allocation_info, nullptr, &resource.memory);
        if (result != VK_SUCCESS) {
            destroy_buffer_resource(resource);
            return core::Result<rhi::RenderBufferCreateStats>::failure(
                "renderer.vulkan_buffer_memory_failed",
                "failed to allocate Vulkan buffer memory: " + std::string(vk_result_name(result)));
        }
        result = vkBindBufferMemory(device_, resource.buffer, resource.memory, 0);
        if (result != VK_SUCCESS) {
            destroy_buffer_resource(resource);
            return core::Result<rhi::RenderBufferCreateStats>::failure(
                "renderer.vulkan_buffer_bind_failed",
                "failed to bind Vulkan buffer memory: " + std::string(vk_result_name(result)));
        }

        const rhi::RenderResourceHandle handle{next_resource_id_++};
        buffer_resources_.emplace(handle.value, std::move(resource));
        rhi::RenderBufferCreateStats stats;
        stats.backend = backend();
        stats.handle = handle;
        stats.usage = desc.usage;
        stats.memory = desc.memory;
        stats.byte_size = desc.byte_size;
        stats.live_resource_count = live_resource_count();
        stats.gpu_backed = true;
        return core::Result<rhi::RenderBufferCreateStats>::success(stats);
    }

    [[nodiscard]] core::Result<rhi::RenderBufferBatchUploadStats>
    upload_buffer_batch(std::span<const rhi::RenderBufferWrite> writes) override {
        auto status = rhi::validate_render_buffer_writes_shape(writes);
        if (!status) {
            return core::Result<rhi::RenderBufferBatchUploadStats>::failure(status.error().code,
                                                                            status.error().message);
        }

        std::size_t staging_bytes = 0;
        for (const auto& write : writes) {
            const auto destination = buffer_resources_.find(write.destination.value);
            if (destination == buffer_resources_.end()) {
                return core::Result<rhi::RenderBufferBatchUploadStats>::failure(
                    "renderer.unknown_buffer_write_destination",
                    "buffer write references a resource not owned by this Vulkan device");
            }
            if (write.destination_offset > destination->second.byte_size ||
                write.bytes.size() > destination->second.byte_size - write.destination_offset) {
                return core::Result<rhi::RenderBufferBatchUploadStats>::failure(
                    "renderer.buffer_write_out_of_bounds",
                    "buffer write exceeds its Vulkan destination resource");
            }
            if (write.destination_offset % 4U != 0 || write.bytes.size() % 4U != 0) {
                return core::Result<rhi::RenderBufferBatchUploadStats>::failure(
                    "renderer.vulkan_buffer_write_alignment",
                    "Vulkan transfer writes require four-byte destination and size alignment");
            }
            if (staging_bytes > std::numeric_limits<std::size_t>::max() - write.bytes.size()) {
                return core::Result<rhi::RenderBufferBatchUploadStats>::failure(
                    "renderer.buffer_write_batch_too_large",
                    "Vulkan buffer write batch byte size overflows size_t");
            }
            staging_bytes += write.bytes.size();
        }

        if (upload_contexts_.empty()) {
            return core::Result<rhi::RenderBufferBatchUploadStats>::failure(
                "renderer.vulkan_upload_context_unavailable",
                "Vulkan upload contexts were not initialized");
        }
        auto& upload_context = upload_contexts_[next_upload_context_];
        double gpu_wait_ms = 0.0;
        status = wait_for_upload_context(upload_context, gpu_wait_ms);
        if (!status) {
            return core::Result<rhi::RenderBufferBatchUploadStats>::failure(status.error().code,
                                                                            status.error().message);
        }

        constexpr std::size_t persistent_staging_capacity = 32U * 1024U * 1024U;
        const auto upload_serial = last_submission_serial_ + 1;
        auto ring_range =
            staging_ring_.allocate(staging_bytes, 4, upload_serial, completed_submission_serial_);
        const bool use_fallback = !ring_range;
        VkBuffer staging_buffer = VK_NULL_HANDLE;
        void* mapped = nullptr;
        std::size_t staging_base_offset = 0;
        if (use_fallback) {
            auto fallback_status = create_staging_buffer(
                staging_bytes, upload_context.fallback_staging, upload_context.fallback_mapped);
            if (!fallback_status) {
                return core::Result<rhi::RenderBufferBatchUploadStats>::failure(
                    fallback_status.error().code, fallback_status.error().message);
            }
            staging_buffer = upload_context.fallback_staging.buffer;
            mapped = upload_context.fallback_mapped;
        } else {
            auto staging_status = ensure_staging_buffer(persistent_staging_capacity);
            if (!staging_status) {
                (void)staging_ring_.cancel(ring_range.value());
                return core::Result<rhi::RenderBufferBatchUploadStats>::failure(
                    staging_status.error().code, staging_status.error().message);
            }
            staging_buffer = staging_buffer_.buffer;
            mapped = staging_mapped_;
            staging_base_offset = ring_range.value().offset;
        }

        std::vector<VkBufferCopy> copy_regions;
        copy_regions.reserve(writes.size());
        std::size_t source_offset = 0;
        for (const auto& write : writes) {
            std::memcpy(static_cast<std::byte*>(mapped) + staging_base_offset + source_offset,
                        write.bytes.data(), write.bytes.size());
            copy_regions.push_back(
                VkBufferCopy{static_cast<VkDeviceSize>(staging_base_offset + source_offset),
                             static_cast<VkDeviceSize>(write.destination_offset),
                             static_cast<VkDeviceSize>(write.bytes.size())});
            source_offset += write.bytes.size();
        }

        VkCommandBufferBeginInfo begin_info{};
        begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        auto result = vkBeginCommandBuffer(upload_context.transfer_commands, &begin_info);
        if (result != VK_SUCCESS) {
            if (use_fallback) {
                vkUnmapMemory(device_, upload_context.fallback_staging.memory);
                upload_context.fallback_mapped = nullptr;
                destroy_buffer_resource(upload_context.fallback_staging);
            } else {
                (void)staging_ring_.cancel(ring_range.value());
            }
            return core::Result<rhi::RenderBufferBatchUploadStats>::failure(
                "renderer.vulkan_buffer_upload_begin_failed",
                "failed to begin Vulkan buffer upload commands: " +
                    std::string(vk_result_name(result)));
        }

        if (upload_context.timestamp_queries != VK_NULL_HANDLE) {
            vkCmdResetQueryPool(upload_context.transfer_commands, upload_context.timestamp_queries,
                                0, 2);
            vkCmdWriteTimestamp(upload_context.transfer_commands, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                upload_context.timestamp_queries, 0);
        }
        begin_debug_label(upload_context.transfer_commands, "Batched buffer upload", 0.92F, 0.56F,
                          0.16F);
        for (std::size_t index = 0; index < writes.size(); ++index) {
            const auto& destination = buffer_resources_.at(writes[index].destination.value);
            vkCmdCopyBuffer(upload_context.transfer_commands, staging_buffer, destination.buffer, 1,
                            &copy_regions[index]);
        }
        std::vector<VkBufferMemoryBarrier> barriers;
        barriers.reserve(writes.size());
        VkPipelineStageFlags destination_stages = 0;
        for (const auto& write : writes) {
            const auto& destination = buffer_resources_.at(write.destination.value);
            VkBufferMemoryBarrier barrier{};
            barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
            barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            switch (destination.desc.usage) {
            case rhi::RenderBufferUsage::vertex:
                barrier.dstAccessMask = VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT;
                destination_stages |= VK_PIPELINE_STAGE_VERTEX_INPUT_BIT;
                break;
            case rhi::RenderBufferUsage::index:
                barrier.dstAccessMask = VK_ACCESS_INDEX_READ_BIT;
                destination_stages |= VK_PIPELINE_STAGE_VERTEX_INPUT_BIT;
                break;
            case rhi::RenderBufferUsage::uniform:
                barrier.dstAccessMask = VK_ACCESS_UNIFORM_READ_BIT;
                destination_stages |= VK_PIPELINE_STAGE_VERTEX_SHADER_BIT |
                                      VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                                      VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
                break;
            case rhi::RenderBufferUsage::storage:
                barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
                destination_stages |= VK_PIPELINE_STAGE_VERTEX_SHADER_BIT |
                                      VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                                      VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
                break;
            }
            barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.buffer = destination.buffer;
            barrier.offset = static_cast<VkDeviceSize>(write.destination_offset);
            barrier.size = static_cast<VkDeviceSize>(write.bytes.size());
            barriers.push_back(barrier);
        }
        vkCmdPipelineBarrier(
            upload_context.transfer_commands, VK_PIPELINE_STAGE_TRANSFER_BIT, destination_stages, 0,
            0, nullptr, static_cast<std::uint32_t>(barriers.size()), barriers.data(), 0, nullptr);
        if (upload_context.timestamp_queries != VK_NULL_HANDLE) {
            vkCmdWriteTimestamp(upload_context.transfer_commands,
                                VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                                upload_context.timestamp_queries, 1);
        }
        end_debug_label(upload_context.transfer_commands);
        result = vkEndCommandBuffer(upload_context.transfer_commands);
        if (result == VK_SUCCESS) {
            VkSubmitInfo submit_info{};
            submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
            submit_info.commandBufferCount = 1;
            submit_info.pCommandBuffers = &upload_context.transfer_commands;
            result = vkQueueSubmit(queue_, 1, &submit_info, upload_context.completion_fence);
        }
        if (result != VK_SUCCESS) {
            if (use_fallback) {
                vkUnmapMemory(device_, upload_context.fallback_staging.memory);
                upload_context.fallback_mapped = nullptr;
                destroy_buffer_resource(upload_context.fallback_staging);
            } else {
                (void)staging_ring_.cancel(ring_range.value());
            }
            return core::Result<rhi::RenderBufferBatchUploadStats>::failure(
                "renderer.vulkan_buffer_upload_failed",
                "failed to submit Vulkan buffer upload: " + std::string(vk_result_name(result)));
        }

        last_submission_serial_ = upload_serial;
        upload_context.submission_serial = upload_serial;
        upload_context.in_flight = true;
        upload_context.timing_pending = upload_context.timestamp_queries != VK_NULL_HANDLE;
        next_upload_context_ = (next_upload_context_ + 1) % upload_contexts_.size();

        rhi::RenderBufferBatchUploadStats stats;
        stats.backend = backend();
        stats.write_count = writes.size();
        stats.byte_size = staging_bytes;
        stats.submission_serial = upload_serial;
        stats.cpu_gpu_wait_ms = gpu_wait_ms;
        stats.used_fallback_staging = use_fallback;
        stats.gpu_backed = true;
        return core::Result<rhi::RenderBufferBatchUploadStats>::success(stats);
    }

    [[nodiscard]] core::Result<rhi::RenderImageUploadStats>
    upload_image(rhi::RenderImageDesc desc, std::span<const std::byte> bytes) override {
        auto status = rhi::validate_render_image_upload(desc, bytes);
        if (!status) {
            return core::Result<rhi::RenderImageUploadStats>::failure(status.error().code,
                                                                      status.error().message);
        }

        const auto format = vulkan_image_format(desc.format);
        if (format == VK_FORMAT_UNDEFINED) {
            return core::Result<rhi::RenderImageUploadStats>::failure(
                "renderer.vulkan_unknown_image_format", "unknown Vulkan image format");
        }
        VkFormatProperties format_properties{};
        vkGetPhysicalDeviceFormatProperties(physical_device_, format, &format_properties);
        const auto required_features =
            VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
        if ((format_properties.optimalTilingFeatures & required_features) != required_features) {
            return core::Result<rhi::RenderImageUploadStats>::failure(
                "renderer.vulkan_image_format_unsupported",
                "Vulkan device does not support sampled transfer uploads for this image format");
        }

        if (upload_contexts_.empty()) {
            return core::Result<rhi::RenderImageUploadStats>::failure(
                "renderer.vulkan_upload_context_unavailable",
                "Vulkan upload contexts were not initialized");
        }
        auto& upload_context = upload_contexts_[next_upload_context_];
        double gpu_wait_ms = 0.0;
        status = wait_for_upload_context(upload_context, gpu_wait_ms);
        if (!status) {
            return core::Result<rhi::RenderImageUploadStats>::failure(status.error().code,
                                                                      status.error().message);
        }

        constexpr std::size_t persistent_staging_capacity = 32U * 1024U * 1024U;
        const auto upload_serial = last_submission_serial_ + 1;
        auto ring_range =
            staging_ring_.allocate(bytes.size(), 4, upload_serial, completed_submission_serial_);
        const bool use_fallback = !ring_range;
        VkBuffer staging_buffer = VK_NULL_HANDLE;
        void* mapped = nullptr;
        std::size_t staging_offset = 0;
        if (use_fallback) {
            auto fallback_status = create_staging_buffer(
                bytes.size(), upload_context.fallback_staging, upload_context.fallback_mapped);
            if (!fallback_status) {
                return core::Result<rhi::RenderImageUploadStats>::failure(
                    fallback_status.error().code, fallback_status.error().message);
            }
            staging_buffer = upload_context.fallback_staging.buffer;
            mapped = upload_context.fallback_mapped;
        } else {
            auto staging_status = ensure_staging_buffer(persistent_staging_capacity);
            if (!staging_status) {
                (void)staging_ring_.cancel(ring_range.value());
                return core::Result<rhi::RenderImageUploadStats>::failure(
                    staging_status.error().code, staging_status.error().message);
            }
            staging_buffer = staging_buffer_.buffer;
            mapped = staging_mapped_;
            staging_offset = ring_range.value().offset;
        }
        std::memcpy(static_cast<std::byte*>(mapped) + staging_offset, bytes.data(), bytes.size());

        const auto release_staging_after_failure = [&]() noexcept {
            if (use_fallback) {
                if (upload_context.fallback_mapped != nullptr &&
                    upload_context.fallback_staging.memory != VK_NULL_HANDLE) {
                    vkUnmapMemory(device_, upload_context.fallback_staging.memory);
                    upload_context.fallback_mapped = nullptr;
                }
                destroy_buffer_resource(upload_context.fallback_staging);
            } else {
                (void)staging_ring_.cancel(ring_range.value());
            }
        };

        VulkanImageResource resource;
        resource.desc = desc;
        resource.byte_size = bytes.size();

        VkImageCreateInfo image_info{};
        image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        image_info.imageType = VK_IMAGE_TYPE_2D;
        image_info.format = format;
        image_info.extent = VkExtent3D{desc.width, desc.height, 1};
        image_info.mipLevels = desc.mip_levels;
        image_info.arrayLayers = desc.array_layers;
        image_info.samples = VK_SAMPLE_COUNT_1_BIT;
        image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
        image_info.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

        auto result = vkCreateImage(device_, &image_info, nullptr, &resource.image);
        if (result != VK_SUCCESS) {
            release_staging_after_failure();
            return core::Result<rhi::RenderImageUploadStats>::failure(
                "renderer.vulkan_sampled_image_failed",
                "failed to create Vulkan sampled image: " + std::string(vk_result_name(result)));
        }

        VkMemoryRequirements image_requirements{};
        vkGetImageMemoryRequirements(device_, resource.image, &image_requirements);
        auto image_memory_type =
            find_memory_type(physical_device_, image_requirements.memoryTypeBits,
                             VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (!image_memory_type) {
            destroy_image_resource(resource);
            release_staging_after_failure();
            return core::Result<rhi::RenderImageUploadStats>::failure(
                image_memory_type.error().code, image_memory_type.error().message);
        }

        VkMemoryAllocateInfo image_allocation{};
        image_allocation.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        image_allocation.allocationSize = image_requirements.size;
        image_allocation.memoryTypeIndex = image_memory_type.value();

        result = vkAllocateMemory(device_, &image_allocation, nullptr, &resource.memory);
        if (result != VK_SUCCESS) {
            destroy_image_resource(resource);
            release_staging_after_failure();
            return core::Result<rhi::RenderImageUploadStats>::failure(
                "renderer.vulkan_sampled_image_memory_failed",
                "failed to allocate Vulkan sampled image memory: " +
                    std::string(vk_result_name(result)));
        }

        result = vkBindImageMemory(device_, resource.image, resource.memory, 0);
        if (result != VK_SUCCESS) {
            destroy_image_resource(resource);
            release_staging_after_failure();
            return core::Result<rhi::RenderImageUploadStats>::failure(
                "renderer.vulkan_sampled_image_bind_failed",
                "failed to bind Vulkan sampled image memory: " +
                    std::string(vk_result_name(result)));
        }

        VkImageViewCreateInfo view_info{};
        view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        view_info.image = resource.image;
        view_info.viewType =
            desc.array_layers > 1 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
        view_info.format = format;
        view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        view_info.subresourceRange.baseMipLevel = 0;
        view_info.subresourceRange.levelCount = desc.mip_levels;
        view_info.subresourceRange.baseArrayLayer = 0;
        view_info.subresourceRange.layerCount = desc.array_layers;
        result = vkCreateImageView(device_, &view_info, nullptr, &resource.image_view);
        if (result != VK_SUCCESS) {
            destroy_image_resource(resource);
            release_staging_after_failure();
            return core::Result<rhi::RenderImageUploadStats>::failure(
                "renderer.vulkan_sampled_image_view_failed",
                "failed to create Vulkan sampled image view: " +
                    std::string(vk_result_name(result)));
        }

        auto sampler_status = ensure_default_sampler();
        if (!sampler_status) {
            destroy_image_resource(resource);
            release_staging_after_failure();
            return core::Result<rhi::RenderImageUploadStats>::failure(
                sampler_status.error().code, sampler_status.error().message);
        }
        resource.sampler = default_sampler_;

        status = upload_sampled_image(upload_context, staging_buffer, staging_offset, resource,
                                      upload_serial);
        if (!status) {
            release_staging_after_failure();
            destroy_image_resource(resource);
            return core::Result<rhi::RenderImageUploadStats>::failure(status.error().code,
                                                                      status.error().message);
        }
        next_upload_context_ = (next_upload_context_ + 1) % upload_contexts_.size();

        const rhi::RenderResourceHandle handle{next_resource_id_++};
        image_resources_.emplace(handle.value, std::move(resource));

        rhi::RenderImageUploadStats stats;
        stats.backend = backend();
        stats.handle = handle;
        stats.format = desc.format;
        stats.width = desc.width;
        stats.height = desc.height;
        stats.array_layers = desc.array_layers;
        stats.mip_levels = desc.mip_levels;
        stats.byte_size = bytes.size();
        stats.live_resource_count = live_resource_count();
        stats.cpu_gpu_wait_ms = gpu_wait_ms;
        stats.gpu_backed = true;
        return core::Result<rhi::RenderImageUploadStats>::success(stats);
    }

    [[nodiscard]] core::Result<rhi::RenderSamplerCreateStats>
    create_sampler(rhi::RenderSamplerDesc desc) override {
        auto status = rhi::validate_render_sampler_desc(desc);
        if (!status) {
            return core::Result<rhi::RenderSamplerCreateStats>::failure(status.error().code,
                                                                        status.error().message);
        }
        VkSamplerCreateInfo sampler_info{};
        sampler_info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        sampler_info.magFilter = vulkan_sampler_filter(desc.mag_filter);
        sampler_info.minFilter = vulkan_sampler_filter(desc.min_filter);
        sampler_info.mipmapMode = vulkan_sampler_mipmap_mode(desc.mipmap_mode);
        sampler_info.addressModeU = vulkan_sampler_address_mode(desc.address_u);
        sampler_info.addressModeV = vulkan_sampler_address_mode(desc.address_v);
        sampler_info.addressModeW = vulkan_sampler_address_mode(desc.address_w);
        sampler_info.maxAnisotropy = desc.max_anisotropy;
        sampler_info.minLod = desc.min_lod;
        sampler_info.maxLod = desc.max_lod;

        VulkanSamplerResource resource;
        resource.desc = std::move(desc);
        const auto result = vkCreateSampler(device_, &sampler_info, nullptr, &resource.sampler);
        if (result != VK_SUCCESS) {
            return core::Result<rhi::RenderSamplerCreateStats>::failure(
                "renderer.vulkan_sampler_failed",
                "failed to create Vulkan sampler: " + std::string(vk_result_name(result)));
        }
        const rhi::RenderResourceHandle handle{next_resource_id_++};
        sampler_resources_.emplace(handle.value, std::move(resource));
        return core::Result<rhi::RenderSamplerCreateStats>::success(
            {backend(), handle, live_resource_count(), true});
    }

    [[nodiscard]] core::Result<rhi::RenderShaderModuleStats>
    create_shader_module(rhi::RenderShaderModuleDesc desc,
                         std::span<const std::uint32_t> spirv_words) override {
        auto status = rhi::validate_render_shader_module_upload(desc, spirv_words);
        if (!status) {
            return core::Result<rhi::RenderShaderModuleStats>::failure(status.error().code,
                                                                       status.error().message);
        }

        VkShaderModuleCreateInfo module_info{};
        module_info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        module_info.codeSize = spirv_words.size() * sizeof(std::uint32_t);
        module_info.pCode = spirv_words.data();

        VulkanShaderModuleResource resource;
        resource.desc = std::move(desc);
        resource.word_count = spirv_words.size();

        const auto result =
            vkCreateShaderModule(device_, &module_info, nullptr, &resource.shader_module);
        if (result != VK_SUCCESS) {
            return core::Result<rhi::RenderShaderModuleStats>::failure(
                "renderer.vulkan_shader_module_failed",
                "failed to create Vulkan shader module: " + std::string(vk_result_name(result)));
        }

        const rhi::RenderResourceHandle handle{next_resource_id_++};
        shader_modules_.emplace(handle.value, resource);

        rhi::RenderShaderModuleStats stats;
        stats.backend = backend();
        stats.handle = handle;
        stats.stage = resource.desc.stage;
        stats.word_count = resource.word_count;
        stats.live_shader_module_count = shader_modules_.size();
        stats.gpu_backed = true;
        return core::Result<rhi::RenderShaderModuleStats>::success(stats);
    }

    [[nodiscard]] core::Result<rhi::RenderPipelineLayoutStats>
    bind_pipeline_layout(rhi::RenderPipelineLayoutDesc desc) override {
        auto status = rhi::validate_render_pipeline_layout_shape(desc);
        if (!status) {
            return core::Result<rhi::RenderPipelineLayoutStats>::failure(status.error().code,
                                                                         status.error().message);
        }

        rhi::RenderPipelineLayoutStats stats;
        stats.backend = backend();
        stats.material_id = desc.material_id;
        stats.pipeline_version = desc.pipeline_version;
        stats.descriptor_count = desc.descriptors.size();
        stats.sampled_texture_count = static_cast<std::size_t>(std::ranges::count_if(
            desc.descriptors, [](const rhi::RenderDescriptorBinding& binding) {
                return binding.kind == rhi::RenderDescriptorKind::sampled_texture;
            }));
        stats.storage_buffer_count = static_cast<std::size_t>(std::ranges::count_if(
            desc.descriptors, [](const rhi::RenderDescriptorBinding& binding) {
                return binding.kind == rhi::RenderDescriptorKind::storage_buffer;
            }));
        stats.uniform_count =
            desc.descriptors.size() - stats.sampled_texture_count - stats.storage_buffer_count;
        stats.push_constant_range_count = desc.push_constant_ranges.size();
        stats.gpu_backed = true;

        const auto key = stats.material_id.value();
        poll_completed_submissions();
        const auto existing = pipeline_layouts_.find(key);
        if (existing != pipeline_layouts_.end() &&
            rhi::equivalent_render_pipeline_layout(existing->second.desc, desc)) {
            stats.bound_pipeline_count = pipeline_layouts_.size();
            return core::Result<rhi::RenderPipelineLayoutStats>::success(stats);
        }
        if (existing != pipeline_layouts_.end() &&
            existing->second.last_used_submission_serial > completed_submission_serial_) {
            return core::Result<rhi::RenderPipelineLayoutStats>::failure(
                "renderer.descriptor_set_in_flight",
                "cannot replace a pipeline layout while its descriptor set is in flight");
        }

        auto resource = create_pipeline_layout_resource(std::move(desc));
        if (!resource) {
            return core::Result<rhi::RenderPipelineLayoutStats>::failure(resource.error().code,
                                                                         resource.error().message);
        }

        destroy_compute_pipelines_for_material(stats.material_id);
        destroy_graphics_pipelines_for_material(stats.material_id);
        destroy_descriptor_writes_for_material(stats.material_id);
        auto replacement = pipeline_layouts_.find(key);
        if (replacement != pipeline_layouts_.end()) {
            destroy_pipeline_layout_resource(replacement->second);
            replacement->second = std::move(resource.value());
        } else {
            pipeline_layouts_.emplace(key, std::move(resource.value()));
        }
        stats.bound_pipeline_count = pipeline_layouts_.size();
        return core::Result<rhi::RenderPipelineLayoutStats>::success(stats);
    }

    [[nodiscard]] core::Result<rhi::RenderComputePipelineStats>
    create_compute_pipeline(rhi::RenderComputePipelineDesc desc) override {
        auto status = rhi::validate_render_compute_pipeline_shape(desc);
        if (!status) {
            return core::Result<rhi::RenderComputePipelineStats>::failure(status.error().code,
                                                                          status.error().message);
        }

        const auto shader_module = shader_modules_.find(desc.compute_shader.value);
        if (shader_module == shader_modules_.end()) {
            return core::Result<rhi::RenderComputePipelineStats>::failure(
                "renderer.unknown_shader_module",
                "compute pipeline references a shader module handle not owned by this device");
        }
        if (shader_module->second.desc.stage != rhi::RenderShaderStage::compute) {
            return core::Result<rhi::RenderComputePipelineStats>::failure(
                "renderer.invalid_compute_shader_stage",
                "compute pipeline shader module must have compute stage");
        }
        const auto pipeline_layout = pipeline_layouts_.find(desc.material_id.value());
        if (pipeline_layout == pipeline_layouts_.end()) {
            return core::Result<rhi::RenderComputePipelineStats>::failure(
                "renderer.unbound_compute_pipeline_layout",
                "compute pipeline material must have a bound pipeline layout");
        }

        VkPipelineShaderStageCreateInfo stage_info{};
        stage_info.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stage_info.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        stage_info.module = shader_module->second.shader_module;
        stage_info.pName = desc.entry_point.c_str();

        VkComputePipelineCreateInfo pipeline_info{};
        pipeline_info.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        pipeline_info.stage = stage_info;
        pipeline_info.layout = pipeline_layout->second.pipeline_layout;

        VulkanComputePipelineResource resource;
        resource.desc = desc;
        const auto result = vkCreateComputePipelines(device_, VK_NULL_HANDLE, 1, &pipeline_info,
                                                     nullptr, &resource.pipeline);
        if (result != VK_SUCCESS) {
            return core::Result<rhi::RenderComputePipelineStats>::failure(
                "renderer.vulkan_compute_pipeline_failed",
                "failed to create Vulkan compute pipeline: " + std::string(vk_result_name(result)));
        }

        const rhi::RenderResourceHandle handle{next_resource_id_++};
        compute_pipelines_.emplace(handle.value, std::move(resource));

        rhi::RenderComputePipelineStats stats;
        stats.backend = backend();
        stats.handle = handle;
        stats.compute_shader = desc.compute_shader;
        stats.material_id = desc.material_id;
        stats.live_compute_pipeline_count = compute_pipelines_.size();
        stats.gpu_backed = true;
        return core::Result<rhi::RenderComputePipelineStats>::success(stats);
    }

    [[nodiscard]] core::Result<rhi::RenderGraphicsPipelineStats>
    create_graphics_pipeline(rhi::RenderGraphicsPipelineDesc desc) override {
        auto status = rhi::validate_render_graphics_pipeline_shape(desc);
        if (!status) {
            return core::Result<rhi::RenderGraphicsPipelineStats>::failure(status.error().code,
                                                                           status.error().message);
        }

        const auto vertex_shader = shader_modules_.find(desc.vertex_shader.value);
        if (vertex_shader == shader_modules_.end()) {
            return core::Result<rhi::RenderGraphicsPipelineStats>::failure(
                "renderer.unknown_vertex_shader",
                "graphics pipeline references a vertex shader handle not owned by this device");
        }
        if (vertex_shader->second.desc.stage != rhi::RenderShaderStage::vertex) {
            return core::Result<rhi::RenderGraphicsPipelineStats>::failure(
                "renderer.invalid_vertex_shader_stage",
                "graphics pipeline vertex shader must have vertex stage");
        }
        const auto fragment_shader = shader_modules_.find(desc.fragment_shader.value);
        if (fragment_shader == shader_modules_.end()) {
            return core::Result<rhi::RenderGraphicsPipelineStats>::failure(
                "renderer.unknown_fragment_shader",
                "graphics pipeline references a fragment shader handle not owned by this device");
        }
        if (fragment_shader->second.desc.stage != rhi::RenderShaderStage::fragment) {
            return core::Result<rhi::RenderGraphicsPipelineStats>::failure(
                "renderer.invalid_fragment_shader_stage",
                "graphics pipeline fragment shader must have fragment stage");
        }
        const auto pipeline_layout = pipeline_layouts_.find(desc.material_id.value());
        if (pipeline_layout == pipeline_layouts_.end()) {
            return core::Result<rhi::RenderGraphicsPipelineStats>::failure(
                "renderer.unbound_graphics_pipeline_layout",
                "graphics pipeline material must have a bound pipeline layout");
        }

        VulkanGraphicsPipelineResource resource;
        resource.desc = desc;
        resource.uses_depth = desc.depth_test_enable || desc.depth_write_enable;

        // Dynamic rendering: the pipeline records the attachment formats it is compatible with,
        // and the frame graph supplies the actual images at vkCmdBeginRendering time. Pipelines are
        // therefore no longer tied to one render pass object, which is what lets scene passes
        // target an HDR image while the tone mapping pass targets the swapchain.
        resource.color_format = vulkan_image_format(desc.color_target_format);
        resource.depth_format = resource.uses_depth ? depth_format_ : VK_FORMAT_UNDEFINED;

        VkPipelineRenderingCreateInfo rendering_info{};
        rendering_info.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
        rendering_info.colorAttachmentCount = 1;
        rendering_info.pColorAttachmentFormats = &resource.color_format;
        rendering_info.depthAttachmentFormat = resource.depth_format;

        const std::array<VkPipelineShaderStageCreateInfo, 2> shader_stages{
            VkPipelineShaderStageCreateInfo{
                VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                nullptr,
                0,
                VK_SHADER_STAGE_VERTEX_BIT,
                vertex_shader->second.shader_module,
                desc.vertex_entry_point.c_str(),
                nullptr,
            },
            VkPipelineShaderStageCreateInfo{
                VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                nullptr,
                0,
                VK_SHADER_STAGE_FRAGMENT_BIT,
                fragment_shader->second.shader_module,
                desc.fragment_entry_point.c_str(),
                nullptr,
            },
        };

        VkPipelineVertexInputStateCreateInfo vertex_input{};
        vertex_input.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        VkVertexInputBindingDescription vertex_binding{};
        std::vector<VkVertexInputAttributeDescription> vertex_attributes;
        if (!desc.vertex_attributes.empty()) {
            vertex_binding.binding = 0;
            vertex_binding.stride = desc.vertex_stride;
            vertex_binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
            vertex_attributes.reserve(desc.vertex_attributes.size());
            for (const auto& attribute : desc.vertex_attributes) {
                VkFormat format = VK_FORMAT_R32G32B32_SFLOAT;
                switch (attribute.format) {
                case rhi::RenderVertexAttributeFormat::float2:
                    format = VK_FORMAT_R32G32_SFLOAT;
                    break;
                case rhi::RenderVertexAttributeFormat::float3:
                    format = VK_FORMAT_R32G32B32_SFLOAT;
                    break;
                case rhi::RenderVertexAttributeFormat::float4:
                    format = VK_FORMAT_R32G32B32A32_SFLOAT;
                    break;
                case rhi::RenderVertexAttributeFormat::sint16x4:
                    format = VK_FORMAT_R16G16B16A16_SINT;
                    break;
                case rhi::RenderVertexAttributeFormat::uint16x4:
                    format = VK_FORMAT_R16G16B16A16_UINT;
                    break;
                case rhi::RenderVertexAttributeFormat::uint16x2:
                    format = VK_FORMAT_R16G16_UINT;
                    break;
                case rhi::RenderVertexAttributeFormat::uint16:
                    format = VK_FORMAT_R16_UINT;
                    break;
                case rhi::RenderVertexAttributeFormat::snorm16x4:
                    format = VK_FORMAT_R16G16B16A16_SNORM;
                    break;
                case rhi::RenderVertexAttributeFormat::unorm16x4:
                    format = VK_FORMAT_R16G16B16A16_UNORM;
                    break;
                case rhi::RenderVertexAttributeFormat::snorm8x4:
                    format = VK_FORMAT_R8G8B8A8_SNORM;
                    break;
                case rhi::RenderVertexAttributeFormat::unorm8x4:
                    format = VK_FORMAT_R8G8B8A8_UNORM;
                    break;
                case rhi::RenderVertexAttributeFormat::uint8x4:
                    format = VK_FORMAT_R8G8B8A8_UINT;
                    break;
                case rhi::RenderVertexAttributeFormat::uint8:
                    format = VK_FORMAT_R8_UINT;
                    break;
                }
                vertex_attributes.push_back({attribute.location, 0, format, attribute.byte_offset});
            }
            vertex_input.vertexBindingDescriptionCount = 1;
            vertex_input.pVertexBindingDescriptions = &vertex_binding;
            vertex_input.vertexAttributeDescriptionCount =
                static_cast<std::uint32_t>(vertex_attributes.size());
            vertex_input.pVertexAttributeDescriptions = vertex_attributes.data();
        }

        VkPipelineInputAssemblyStateCreateInfo input_assembly{};
        input_assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        input_assembly.topology = vulkan_primitive_topology(desc.topology);

        VkPipelineViewportStateCreateInfo viewport_state{};
        viewport_state.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
        viewport_state.viewportCount = 1;
        viewport_state.scissorCount = 1;

        VkPipelineRasterizationStateCreateInfo rasterization{};
        rasterization.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        rasterization.polygonMode = vulkan_polygon_mode(desc.polygon_mode);
        rasterization.cullMode = vulkan_cull_mode(desc.cull_mode);
        rasterization.frontFace = vulkan_front_face(desc.front_face);
        rasterization.lineWidth = 1.0F;

        VkPipelineMultisampleStateCreateInfo multisample{};
        multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

        VkPipelineDepthStencilStateCreateInfo depth_stencil{};
        depth_stencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
        depth_stencil.depthTestEnable = desc.depth_test_enable ? VK_TRUE : VK_FALSE;
        depth_stencil.depthWriteEnable = desc.depth_write_enable ? VK_TRUE : VK_FALSE;
        depth_stencil.depthCompareOp = vulkan_compare_operation(desc.depth_compare);
        depth_stencil.depthBoundsTestEnable = VK_FALSE;
        depth_stencil.stencilTestEnable = VK_FALSE;

        VkPipelineColorBlendAttachmentState color_blend_attachment{};
        color_blend_attachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT |
                                                VK_COLOR_COMPONENT_G_BIT |
                                                VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        if (desc.blend_mode == rhi::RenderBlendMode::alpha) {
            color_blend_attachment.blendEnable = VK_TRUE;
            color_blend_attachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
            color_blend_attachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            color_blend_attachment.colorBlendOp = VK_BLEND_OP_ADD;
            color_blend_attachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
            color_blend_attachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            color_blend_attachment.alphaBlendOp = VK_BLEND_OP_ADD;
        }

        VkPipelineColorBlendStateCreateInfo color_blend{};
        color_blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        color_blend.attachmentCount = 1;
        color_blend.pAttachments = &color_blend_attachment;

        const std::array<VkDynamicState, 2> dynamic_states{
            VK_DYNAMIC_STATE_VIEWPORT,
            VK_DYNAMIC_STATE_SCISSOR,
        };
        VkPipelineDynamicStateCreateInfo dynamic_state{};
        dynamic_state.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
        dynamic_state.dynamicStateCount = static_cast<std::uint32_t>(dynamic_states.size());
        dynamic_state.pDynamicStates = dynamic_states.data();

        VkGraphicsPipelineCreateInfo pipeline_info{};
        pipeline_info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        pipeline_info.stageCount = static_cast<std::uint32_t>(shader_stages.size());
        pipeline_info.pStages = shader_stages.data();
        pipeline_info.pVertexInputState = &vertex_input;
        pipeline_info.pInputAssemblyState = &input_assembly;
        pipeline_info.pViewportState = &viewport_state;
        pipeline_info.pRasterizationState = &rasterization;
        pipeline_info.pMultisampleState = &multisample;
        pipeline_info.pDepthStencilState = &depth_stencil;
        pipeline_info.pColorBlendState = &color_blend;
        pipeline_info.pDynamicState = &dynamic_state;
        pipeline_info.layout = pipeline_layout->second.pipeline_layout;
        pipeline_info.pNext = &rendering_info;
        pipeline_info.renderPass = VK_NULL_HANDLE;
        pipeline_info.subpass = 0;

        const auto result = vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &pipeline_info,
                                                      nullptr, &resource.pipeline);
        if (result != VK_SUCCESS) {
            destroy_graphics_pipeline_resource(resource);
            return core::Result<rhi::RenderGraphicsPipelineStats>::failure(
                "renderer.vulkan_graphics_pipeline_failed",
                "failed to create Vulkan graphics pipeline: " +
                    std::string(vk_result_name(result)));
        }

        const rhi::RenderResourceHandle handle{next_resource_id_++};
        graphics_pipelines_.emplace(handle.value, std::move(resource));

        rhi::RenderGraphicsPipelineStats stats;
        stats.backend = backend();
        stats.handle = handle;
        stats.vertex_shader = desc.vertex_shader;
        stats.fragment_shader = desc.fragment_shader;
        stats.material_id = desc.material_id;
        stats.live_graphics_pipeline_count = graphics_pipelines_.size();
        stats.gpu_backed = true;
        return core::Result<rhi::RenderGraphicsPipelineStats>::success(stats);
    }

    [[nodiscard]] core::Result<rhi::RenderDescriptorWriteStats>
    write_descriptors(std::span<const rhi::RenderDescriptorWrite> writes) override {
        auto status = rhi::validate_render_descriptor_writes_shape(writes);
        if (!status) {
            return core::Result<rhi::RenderDescriptorWriteStats>::failure(status.error().code,
                                                                          status.error().message);
        }
        poll_completed_submissions();

        std::unordered_set<std::string> materials;
        std::size_t uniform_write_count = 0;
        std::size_t sampled_texture_write_count = 0;
        std::size_t storage_buffer_write_count = 0;
        std::vector<VkDescriptorBufferInfo> buffer_infos;
        std::vector<VkDescriptorImageInfo> image_infos;
        std::vector<VkWriteDescriptorSet> descriptor_writes;
        buffer_infos.reserve(writes.size());
        image_infos.reserve(writes.size());
        descriptor_writes.reserve(writes.size());

        for (const auto& write : writes) {
            const auto layout = pipeline_layouts_.find(write.material_id.value());
            if (layout == pipeline_layouts_.end()) {
                return core::Result<rhi::RenderDescriptorWriteStats>::failure(
                    "renderer.unbound_descriptor_layout",
                    "descriptor write material must have a bound pipeline layout");
            }
            if (layout->second.last_used_submission_serial > completed_submission_serial_) {
                return core::Result<rhi::RenderDescriptorWriteStats>::failure(
                    "renderer.descriptor_set_in_flight",
                    "cannot update a descriptor set while it is in flight");
            }
            const auto* binding = find_descriptor_binding(layout->second.desc, write.binding_name);
            if (binding == nullptr) {
                return core::Result<rhi::RenderDescriptorWriteStats>::failure(
                    "renderer.unknown_descriptor_binding",
                    "descriptor write binding does not exist in the material pipeline layout");
            }
            if (binding->kind == rhi::RenderDescriptorKind::sampled_texture) {
                const auto image = image_resources_.find(write.resource.value);
                if (image == image_resources_.end()) {
                    if (buffer_resources_.contains(write.resource.value)) {
                        return core::Result<rhi::RenderDescriptorWriteStats>::failure(
                            "renderer.invalid_descriptor_resource_usage",
                            "sampled texture descriptor writes must reference an image resource");
                    }
                    return core::Result<rhi::RenderDescriptorWriteStats>::failure(
                        "renderer.unknown_descriptor_resource",
                        "descriptor write resource handle is not owned by this device");
                }
                if (write.byte_offset != 0 || write.byte_size != 0) {
                    return core::Result<rhi::RenderDescriptorWriteStats>::failure(
                        "renderer.invalid_sampled_texture_descriptor_range",
                        "sampled texture descriptor writes must not specify a byte range");
                }

                auto sampler = image->second.sampler;
                if (write.sampler.is_valid()) {
                    const auto sampler_resource = sampler_resources_.find(write.sampler.value);
                    if (sampler_resource == sampler_resources_.end()) {
                        return core::Result<rhi::RenderDescriptorWriteStats>::failure(
                            "renderer.unknown_descriptor_sampler",
                            "sampled texture descriptor references an unknown sampler");
                    }
                    sampler = sampler_resource->second.sampler;
                }
                image_infos.push_back(VkDescriptorImageInfo{
                    sampler,
                    image->second.image_view,
                    image->second.layout,
                });
                VkWriteDescriptorSet descriptor_write{};
                descriptor_write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                descriptor_write.dstSet = layout->second.descriptor_set;
                descriptor_write.dstBinding = binding->slot;
                descriptor_write.dstArrayElement = 0;
                descriptor_write.descriptorCount = 1;
                descriptor_write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                descriptor_write.pImageInfo = &image_infos.back();
                descriptor_writes.push_back(descriptor_write);

                materials.insert(write.material_id.value());
                ++sampled_texture_write_count;
                continue;
            }

            const auto resource = buffer_resources_.find(write.resource.value);
            if (resource == buffer_resources_.end()) {
                if (image_resources_.contains(write.resource.value)) {
                    return core::Result<rhi::RenderDescriptorWriteStats>::failure(
                        "renderer.invalid_descriptor_resource_usage",
                        "uniform descriptor writes must reference a uniform buffer resource");
                }
                return core::Result<rhi::RenderDescriptorWriteStats>::failure(
                    "renderer.unknown_descriptor_resource",
                    "descriptor write resource handle is not owned by this device");
            }
            const auto required_usage = binding->kind == rhi::RenderDescriptorKind::storage_buffer
                                            ? rhi::RenderBufferUsage::storage
                                            : rhi::RenderBufferUsage::uniform;
            if (resource->second.desc.usage != required_usage) {
                return core::Result<rhi::RenderDescriptorWriteStats>::failure(
                    "renderer.invalid_descriptor_resource_usage",
                    "buffer descriptor write references an incompatible buffer usage");
            }
            if (write.byte_size == 0) {
                return core::Result<rhi::RenderDescriptorWriteStats>::failure(
                    "renderer.invalid_descriptor_write_size",
                    "uniform descriptor write byte size must be non-zero");
            }
            if (write.byte_offset > resource->second.byte_size ||
                write.byte_size > resource->second.byte_size - write.byte_offset) {
                return core::Result<rhi::RenderDescriptorWriteStats>::failure(
                    "renderer.descriptor_write_out_of_range",
                    "descriptor write byte range must fit inside the referenced resource");
            }

            buffer_infos.push_back(VkDescriptorBufferInfo{
                resource->second.buffer,
                static_cast<VkDeviceSize>(write.byte_offset),
                static_cast<VkDeviceSize>(write.byte_size),
            });
            VkWriteDescriptorSet descriptor_write{};
            descriptor_write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            descriptor_write.dstSet = layout->second.descriptor_set;
            descriptor_write.dstBinding = binding->slot;
            descriptor_write.dstArrayElement = 0;
            descriptor_write.descriptorCount = 1;
            descriptor_write.descriptorType = vulkan_descriptor_type(binding->kind);
            descriptor_write.pBufferInfo = &buffer_infos.back();
            descriptor_writes.push_back(descriptor_write);

            materials.insert(write.material_id.value());
            if (binding->kind == rhi::RenderDescriptorKind::storage_buffer) {
                ++storage_buffer_write_count;
            } else {
                ++uniform_write_count;
            }
        }

        vkUpdateDescriptorSets(device_, static_cast<std::uint32_t>(descriptor_writes.size()),
                               descriptor_writes.data(), 0, nullptr);
        for (const auto& write : writes) {
            descriptor_write_records_.insert_or_assign(
                write.material_id.value() + "|" + write.binding_name, write);
        }

        rhi::RenderDescriptorWriteStats stats;
        stats.backend = backend();
        stats.write_count = writes.size();
        stats.uniform_write_count = uniform_write_count;
        stats.sampled_texture_write_count = sampled_texture_write_count;
        stats.storage_buffer_write_count = storage_buffer_write_count;
        stats.material_count = materials.size();
        stats.gpu_backed = true;
        return core::Result<rhi::RenderDescriptorWriteStats>::success(stats);
    }

    [[nodiscard]] core::Result<rhi::RenderDrawStats>
    bind_mesh_draws(std::span<const rhi::RenderMeshBinding> draws) override {
        auto status = rhi::validate_render_mesh_bindings_shape(draws);
        if (!status) {
            return core::Result<rhi::RenderDrawStats>::failure(status.error().code,
                                                               status.error().message);
        }

        std::unordered_set<std::string> materials;
        rhi::RenderDrawStats stats;
        stats.backend = backend();
        stats.draw_count = draws.size();
        stats.gpu_backed = true;

        for (const auto& draw : draws) {
            const auto has_graphics_pipeline =
                std::ranges::any_of(graphics_pipelines_, [&draw](const auto& pipeline) {
                    return pipeline.second.desc.material_id == draw.material_id;
                });
            if (!has_graphics_pipeline) {
                return core::Result<rhi::RenderDrawStats>::failure(
                    "renderer.unbound_material_graphics_pipeline",
                    "mesh draw material must have a graphics pipeline");
            }
            const auto layout = pipeline_layouts_.find(draw.material_id.value());
            if (layout == pipeline_layouts_.end()) {
                return core::Result<rhi::RenderDrawStats>::failure(
                    "renderer.unbound_graphics_pipeline_layout",
                    "mesh draw graphics pipeline layout is no longer bound");
            }
            status = validate_required_descriptors(layout->second);
            if (!status) {
                return core::Result<rhi::RenderDrawStats>::failure(status.error().code,
                                                                   status.error().message);
            }
            const auto vertex = buffer_resources_.find(draw.vertex_buffer.value);
            if (vertex == buffer_resources_.end()) {
                return core::Result<rhi::RenderDrawStats>::failure(
                    "renderer.unknown_vertex_buffer",
                    "mesh draw references a vertex buffer handle not owned by this device");
            }
            if (vertex->second.desc.usage != rhi::RenderBufferUsage::vertex) {
                return core::Result<rhi::RenderDrawStats>::failure(
                    "renderer.invalid_vertex_buffer_usage",
                    "mesh draw vertex buffer must reference a vertex buffer resource");
            }

            if (draw.index_buffer.is_valid()) {
                const auto index = buffer_resources_.find(draw.index_buffer.value);
                if (index == buffer_resources_.end()) {
                    return core::Result<rhi::RenderDrawStats>::failure(
                        "renderer.unknown_index_buffer",
                        "mesh draw references an index buffer handle not owned by this device");
                }
                if (index->second.desc.usage != rhi::RenderBufferUsage::index) {
                    return core::Result<rhi::RenderDrawStats>::failure(
                        "renderer.invalid_index_buffer_usage",
                        "mesh draw index buffer must reference an index buffer resource");
                }
                ++stats.indexed_draw_count;
            }

            materials.insert(draw.material_id.value());
            stats.total_vertices += draw.vertex_count;
            stats.total_indices += static_cast<std::size_t>(draw.index_count) * draw.instance_count;
        }

        status = submit_offscreen_mesh_draws(draws);
        if (!status) {
            return core::Result<rhi::RenderDrawStats>::failure(status.error().code,
                                                               status.error().message);
        }

        stats.material_count = materials.size();
        stats.draw_commands_submitted = true;
        return core::Result<rhi::RenderDrawStats>::success(stats);
    }

    [[nodiscard]] core::Status release_resource(rhi::RenderResourceHandle handle) override {
        if (!handle.is_valid()) {
            return core::Status::failure("renderer.invalid_resource_handle",
                                         "render resource handle must be valid");
        }
        poll_completed_submissions();
        for (const auto& [_, write] : descriptor_write_records_) {
            if (write.resource.value != handle.value && write.sampler.value != handle.value) {
                continue;
            }
            const auto layout = pipeline_layouts_.find(write.material_id.value());
            if (layout != pipeline_layouts_.end() &&
                layout->second.last_used_submission_serial > completed_submission_serial_) {
                return core::Status::failure(
                    "renderer.descriptor_resource_in_flight",
                    "cannot release a descriptor resource while its descriptor set is in flight");
            }
        }
        auto found = buffer_resources_.find(handle.value);
        if (found != buffer_resources_.end()) {
            destroy_descriptor_writes_for_resource(handle);
            retired_buffers_.push_back({std::move(found->second), last_submission_serial_});
            buffer_resources_.erase(found);
            collect_retired_resources();
            return core::Status::ok();
        }
        auto image = image_resources_.find(handle.value);
        if (image != image_resources_.end()) {
            destroy_descriptor_writes_for_resource(handle);
            retired_images_.push_back({std::move(image->second), last_submission_serial_});
            image_resources_.erase(image);
            collect_retired_resources();
            return core::Status::ok();
        }
        auto sampler = sampler_resources_.find(handle.value);
        if (sampler != sampler_resources_.end()) {
            destroy_descriptor_writes_for_resource(handle);
            retired_samplers_.push_back({std::move(sampler->second), last_submission_serial_});
            sampler_resources_.erase(sampler);
            collect_retired_resources();
            return core::Status::ok();
        }
        auto shader_module = shader_modules_.find(handle.value);
        if (shader_module != shader_modules_.end()) {
            retire_compute_pipelines_for_shader(handle);
            retire_graphics_pipelines_for_shader(handle);
            retired_shader_modules_.push_back(
                {std::move(shader_module->second), last_submission_serial_});
            shader_modules_.erase(shader_module);
            collect_retired_resources();
            return core::Status::ok();
        }
        auto compute_pipeline = compute_pipelines_.find(handle.value);
        if (compute_pipeline != compute_pipelines_.end()) {
            retired_compute_pipelines_.push_back(
                {std::move(compute_pipeline->second), last_submission_serial_});
            compute_pipelines_.erase(compute_pipeline);
            collect_retired_resources();
            return core::Status::ok();
        }
        auto graphics_pipeline = graphics_pipelines_.find(handle.value);
        if (graphics_pipeline != graphics_pipelines_.end()) {
            retired_graphics_pipelines_.push_back(
                {std::move(graphics_pipeline->second), last_submission_serial_});
            graphics_pipelines_.erase(graphics_pipeline);
            collect_retired_resources();
            return core::Status::ok();
        }
        return core::Status::failure("renderer.unknown_resource",
                                     "render resource handle is not owned by this device");
    }

  private:
    [[nodiscard]] core::Status
    validate_required_descriptors(
        const VulkanPipelineLayoutResource& layout,
        std::span<const rhi::RenderPassSampledResource> graph_supplied = {}) const {
        for (const auto& binding : layout.desc.descriptors) {
            if (!binding.required) {
                continue;
            }
            // Bindings the frame graph fills are resolved from the pool every frame rather than
            // written once through write_descriptors, so they are satisfied by construction.
            const auto supplied_by_graph = std::ranges::any_of(
                graph_supplied, [&binding](const rhi::RenderPassSampledResource& sampled) {
                    return sampled.binding_name == binding.name;
                });
            if (supplied_by_graph) {
                continue;
            }
            const auto key = layout.desc.material_id.value() + "|" + binding.name;
            if (!descriptor_write_records_.contains(key)) {
                return core::Status::failure("renderer.required_descriptor_unbound",
                                             "required descriptor binding has not been written: " +
                                                 binding.name);
            }
        }
        return core::Status::ok();
    }

    [[nodiscard]] core::Result<VulkanFrameContext> create_frame_context() {
        VulkanFrameContext context;
        auto pool = create_command_pool(device_, graphics_queue_family_);
        if (!pool) {
            return core::Result<VulkanFrameContext>::failure(pool.error().code,
                                                             pool.error().message);
        }
        context.command_pool = pool.value();
        auto commands = allocate_command_buffer(device_, context.command_pool);
        if (!commands) {
            destroy_frame_context(context);
            return core::Result<VulkanFrameContext>::failure(commands.error().code,
                                                             commands.error().message);
        }
        context.graphics_commands = commands.value();
        auto fence = create_fence(device_);
        if (!fence) {
            destroy_frame_context(context);
            return core::Result<VulkanFrameContext>::failure(fence.error().code,
                                                             fence.error().message);
        }
        context.completion_fence = fence.value();
        auto semaphore = create_semaphore(device_);
        if (!semaphore) {
            destroy_frame_context(context);
            return core::Result<VulkanFrameContext>::failure(semaphore.error().code,
                                                             semaphore.error().message);
        }
        context.image_available = semaphore.value();
        return core::Result<VulkanFrameContext>::success(std::move(context));
    }

    [[nodiscard]] core::Result<VulkanUploadContext> create_upload_context() {
        VulkanUploadContext context;
        auto pool = create_command_pool(device_, graphics_queue_family_);
        if (!pool) {
            return core::Result<VulkanUploadContext>::failure(pool.error().code,
                                                              pool.error().message);
        }
        context.command_pool = pool.value();
        auto commands = allocate_command_buffer(device_, context.command_pool);
        if (!commands) {
            destroy_upload_context(context);
            return core::Result<VulkanUploadContext>::failure(commands.error().code,
                                                              commands.error().message);
        }
        context.transfer_commands = commands.value();
        auto fence = create_fence(device_);
        if (!fence) {
            destroy_upload_context(context);
            return core::Result<VulkanUploadContext>::failure(fence.error().code,
                                                              fence.error().message);
        }
        context.completion_fence = fence.value();
        if (timestamp_valid_bits_ != 0 && properties_.limits.timestampPeriod > 0.0F) {
            VkQueryPoolCreateInfo query_info{};
            query_info.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
            query_info.queryType = VK_QUERY_TYPE_TIMESTAMP;
            query_info.queryCount = 2;
            const auto result =
                vkCreateQueryPool(device_, &query_info, nullptr, &context.timestamp_queries);
            if (result != VK_SUCCESS) {
                context.timestamp_queries = VK_NULL_HANDLE;
                core::log(core::LogLevel::warning, "Vulkan upload timestamp queries unavailable: " +
                                                       std::string(vk_result_name(result)));
            }
        }
        return core::Result<VulkanUploadContext>::success(std::move(context));
    }

    void destroy_frame_target(VulkanFrameTarget& target) noexcept {
        if (target.depth_view != VK_NULL_HANDLE) {
            vkDestroyImageView(device_, target.depth_view, nullptr);
        }
        if (target.depth_image != VK_NULL_HANDLE) {
            vkDestroyImage(device_, target.depth_image, nullptr);
        }
        if (target.depth_memory != VK_NULL_HANDLE) {
            vkFreeMemory(device_, target.depth_memory, nullptr);
        }
        if (target.color_view != VK_NULL_HANDLE) {
            vkDestroyImageView(device_, target.color_view, nullptr);
        }
        if (target.color_image != VK_NULL_HANDLE) {
            vkDestroyImage(device_, target.color_image, nullptr);
        }
        if (target.color_memory != VK_NULL_HANDLE) {
            vkFreeMemory(device_, target.color_memory, nullptr);
        }
        target = {};
    }

    void destroy_frame_context(VulkanFrameContext& context) noexcept {
        destroy_frame_target(context.target);
        if (context.image_available != VK_NULL_HANDLE) {
            vkDestroySemaphore(device_, context.image_available, nullptr);
        }
        if (context.completion_fence != VK_NULL_HANDLE) {
            vkDestroyFence(device_, context.completion_fence, nullptr);
        }
        if (context.command_pool != VK_NULL_HANDLE) {
            vkDestroyCommandPool(device_, context.command_pool, nullptr);
        }
        context = {};
    }

    void destroy_upload_context(VulkanUploadContext& context) noexcept {
        if (context.fallback_mapped != nullptr &&
            context.fallback_staging.memory != VK_NULL_HANDLE) {
            vkUnmapMemory(device_, context.fallback_staging.memory);
        }
        destroy_buffer_resource(context.fallback_staging);
        if (context.timestamp_queries != VK_NULL_HANDLE) {
            vkDestroyQueryPool(device_, context.timestamp_queries, nullptr);
        }
        if (context.completion_fence != VK_NULL_HANDLE) {
            vkDestroyFence(device_, context.completion_fence, nullptr);
        }
        if (context.command_pool != VK_NULL_HANDLE) {
            vkDestroyCommandPool(device_, context.command_pool, nullptr);
        }
        context = {};
    }

    void destroy_frame_contexts() noexcept {
        for (auto& context : frame_contexts_) {
            destroy_frame_context(context);
        }
        frame_contexts_.clear();
        for (auto& context : upload_contexts_) {
            destroy_upload_context(context);
        }
        upload_contexts_.clear();
    }

    void collect_completed_upload_timings() noexcept {
        for (auto& context : upload_contexts_) {
            if (!context.timing_pending ||
                context.submission_serial > completed_submission_serial_) {
                continue;
            }
            std::array<std::uint64_t, 2> values{};
            const auto result =
                vkGetQueryPoolResults(device_, context.timestamp_queries, 0, 2, sizeof(values),
                                      values.data(), sizeof(std::uint64_t), VK_QUERY_RESULT_64_BIT);
            if (result != VK_SUCCESS) {
                continue;
            }
            context.timing_pending = false;
            if (!latest_gpu_upload_timing_valid_ ||
                context.submission_serial >= latest_gpu_upload_submission_serial_) {
                latest_gpu_upload_submission_serial_ = context.submission_serial;
                latest_gpu_upload_ms_ = timestamp_milliseconds(values[0], values[1]);
                latest_gpu_upload_timing_valid_ = true;
            }
        }
    }

    void attach_latest_gpu_upload_timing(rhi::RenderFrameStats& stats) noexcept {
        if (!latest_gpu_upload_timing_valid_) {
            return;
        }
        stats.gpu_upload_timing_valid = true;
        stats.gpu_upload_submission_serial = latest_gpu_upload_submission_serial_;
        stats.gpu_upload_ms = latest_gpu_upload_ms_;
        latest_gpu_upload_timing_valid_ = false;
    }

    void advance_completed_submission(std::uint64_t submission_serial) noexcept {
        completed_submission_serial_ = std::max(completed_submission_serial_, submission_serial);
        while (!pending_frame_submissions_.empty() &&
               pending_frame_submissions_.front() <= completed_submission_serial_) {
            pending_frame_submissions_.pop_front();
            ++completed_frame_count_;
        }
        staging_ring_.release_completed(completed_submission_serial_);
        collect_completed_upload_timings();
        collect_retired_resources();
    }

    void collect_retired_resources() noexcept {
        const auto completed = completed_submission_serial_;
        std::erase_if(retired_graphics_pipelines_, [this, completed](auto& retired) {
            if (retired.retire_after_submission > completed) {
                return false;
            }
            destroy_graphics_pipeline_resource(retired.resource);
            return true;
        });
        std::erase_if(retired_compute_pipelines_, [this, completed](auto& retired) {
            if (retired.retire_after_submission > completed) {
                return false;
            }
            destroy_compute_pipeline_resource(retired.resource);
            return true;
        });
        std::erase_if(retired_shader_modules_, [this, completed](auto& retired) {
            if (retired.retire_after_submission > completed) {
                return false;
            }
            destroy_shader_module_resource(retired.resource);
            return true;
        });
        std::erase_if(retired_images_, [this, completed](auto& retired) {
            if (retired.retire_after_submission > completed) {
                return false;
            }
            destroy_image_resource(retired.resource);
            return true;
        });
        std::erase_if(retired_samplers_, [this, completed](auto& retired) {
            if (retired.retire_after_submission > completed) {
                return false;
            }
            destroy_sampler_resource(retired.resource);
            return true;
        });
        std::erase_if(retired_buffers_, [this, completed](auto& retired) {
            if (retired.retire_after_submission > completed) {
                return false;
            }
            destroy_buffer_resource(retired.resource);
            return true;
        });
    }

    void complete_all_submissions() noexcept {
        std::uint64_t newest = completed_submission_serial_;
        for (auto& context : frame_contexts_) {
            if (context.in_flight) {
                newest = std::max(newest, context.submission_serial);
                context.in_flight = false;
            }
        }
        for (auto& context : upload_contexts_) {
            if (context.in_flight) {
                newest = std::max(newest, context.submission_serial);
                context.in_flight = false;
            }
        }
        advance_completed_submission(newest);
    }

    void poll_completed_submissions() noexcept {
        std::uint64_t newest = completed_submission_serial_;
        for (auto& context : frame_contexts_) {
            if (!context.in_flight ||
                vkGetFenceStatus(device_, context.completion_fence) != VK_SUCCESS) {
                continue;
            }
            newest = std::max(newest, context.submission_serial);
            context.in_flight = false;
        }
        for (auto& context : upload_contexts_) {
            if (!context.in_flight ||
                vkGetFenceStatus(device_, context.completion_fence) != VK_SUCCESS) {
                continue;
            }
            newest = std::max(newest, context.submission_serial);
            context.in_flight = false;
        }
        if (newest > completed_submission_serial_) {
            advance_completed_submission(newest);
        }
    }

    [[nodiscard]] core::Status wait_for_frame_context(VulkanFrameContext& context,
                                                      double& wait_ms) {
        using Clock = std::chrono::steady_clock;
        if (context.in_flight) {
            const auto started = Clock::now();
            const auto result = vkWaitForFences(device_, 1, &context.completion_fence, VK_TRUE,
                                                std::numeric_limits<std::uint64_t>::max());
            wait_ms += std::chrono::duration<double, std::milli>(Clock::now() - started).count();
            if (result != VK_SUCCESS) {
                return core::Status::failure("renderer.vulkan_wait_frame_context_failed",
                                             "failed to wait for a reusable Vulkan frame: " +
                                                 std::string(vk_result_name(result)));
            }
            advance_completed_submission(context.submission_serial);
            context.in_flight = false;
        }
        auto result = vkResetFences(device_, 1, &context.completion_fence);
        if (result == VK_SUCCESS) {
            result = vkResetCommandPool(device_, context.command_pool, 0);
        }
        if (result != VK_SUCCESS) {
            return core::Status::failure("renderer.vulkan_reset_frame_context_failed",
                                         "failed to reset a reusable Vulkan frame: " +
                                             std::string(vk_result_name(result)));
        }
        return core::Status::ok();
    }

    [[nodiscard]] core::Status wait_for_upload_context(VulkanUploadContext& context,
                                                       double& wait_ms) {
        using Clock = std::chrono::steady_clock;
        if (context.in_flight) {
            const auto started = Clock::now();
            const auto result = vkWaitForFences(device_, 1, &context.completion_fence, VK_TRUE,
                                                std::numeric_limits<std::uint64_t>::max());
            wait_ms += std::chrono::duration<double, std::milli>(Clock::now() - started).count();
            if (result != VK_SUCCESS) {
                return core::Status::failure("renderer.vulkan_wait_upload_context_failed",
                                             "failed to wait for a reusable Vulkan upload: " +
                                                 std::string(vk_result_name(result)));
            }
            advance_completed_submission(context.submission_serial);
            context.in_flight = false;
        }
        if (context.fallback_mapped != nullptr &&
            context.fallback_staging.memory != VK_NULL_HANDLE) {
            vkUnmapMemory(device_, context.fallback_staging.memory);
            context.fallback_mapped = nullptr;
        }
        destroy_buffer_resource(context.fallback_staging);
        auto result = vkResetFences(device_, 1, &context.completion_fence);
        if (result == VK_SUCCESS) {
            result = vkResetCommandPool(device_, context.command_pool, 0);
        }
        if (result != VK_SUCCESS) {
            return core::Status::failure("renderer.vulkan_reset_upload_context_failed",
                                         "failed to reset a reusable Vulkan upload: " +
                                             std::string(vk_result_name(result)));
        }
        return core::Status::ok();
    }

    void initialize_instrumentation() noexcept {
        if (debug_utils_enabled_) {
            begin_debug_label_ = reinterpret_cast<PFN_vkCmdBeginDebugUtilsLabelEXT>(
                vkGetDeviceProcAddr(device_, "vkCmdBeginDebugUtilsLabelEXT"));
            end_debug_label_ = reinterpret_cast<PFN_vkCmdEndDebugUtilsLabelEXT>(
                vkGetDeviceProcAddr(device_, "vkCmdEndDebugUtilsLabelEXT"));
        }

        if (timestamp_valid_bits_ == 0 || properties_.limits.timestampPeriod <= 0.0F) {
            core::log(core::LogLevel::warning,
                      "Vulkan queue does not support timestamp-query instrumentation");
            return;
        }
        VkQueryPoolCreateInfo query_info{};
        query_info.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
        query_info.queryType = VK_QUERY_TYPE_TIMESTAMP;
        query_info.queryCount = timestamp_slot_count * timestamps_per_slot;
        const auto result =
            vkCreateQueryPool(device_, &query_info, nullptr, &timestamp_query_pool_);
        if (result != VK_SUCCESS) {
            timestamp_query_pool_ = VK_NULL_HANDLE;
            core::log(core::LogLevel::warning, "Vulkan timestamp query pool unavailable: " +
                                                   std::string(vk_result_name(result)));
        }
    }

    void begin_debug_label(VkCommandBuffer command_buffer, const char* name, float red, float green,
                           float blue) const noexcept {
        if (begin_debug_label_ == nullptr) {
            return;
        }
        VkDebugUtilsLabelEXT label{};
        label.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT;
        label.pLabelName = name;
        label.color[0] = red;
        label.color[1] = green;
        label.color[2] = blue;
        label.color[3] = 1.0F;
        begin_debug_label_(command_buffer, &label);
    }

    void end_debug_label(VkCommandBuffer command_buffer) const noexcept {
        if (end_debug_label_ != nullptr) {
            end_debug_label_(command_buffer);
        }
    }

    [[nodiscard]] std::uint32_t timestamp_base(std::uint64_t frame_index) const noexcept {
        const auto slot = static_cast<std::uint32_t>(frame_index % timestamp_slot_count);
        return slot * timestamps_per_slot;
    }

    void begin_timestamp_frame(VkCommandBuffer command_buffer, std::uint64_t frame_index) noexcept {
        if (timestamp_query_pool_ == VK_NULL_HANDLE) {
            return;
        }
        const auto slot = static_cast<std::size_t>(frame_index % timestamp_slot_count);
        timestamp_pending_[slot] = false;
        timestamp_frame_indices_[slot] = frame_index;
        const auto base = timestamp_base(frame_index);
        vkCmdResetQueryPool(command_buffer, timestamp_query_pool_, base, timestamps_per_slot);
        vkCmdWriteTimestamp(command_buffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                            timestamp_query_pool_, base + frame_start_timestamp);
    }

    void write_timestamp(VkCommandBuffer command_buffer, std::uint64_t frame_index,
                         TimestampIndex index, VkPipelineStageFlagBits stage) const noexcept {
        if (timestamp_query_pool_ == VK_NULL_HANDLE) {
            return;
        }
        vkCmdWriteTimestamp(command_buffer, stage, timestamp_query_pool_,
                            timestamp_base(frame_index) + index);
    }

    void mark_timestamp_frame_pending(std::uint64_t frame_index) noexcept {
        if (timestamp_query_pool_ == VK_NULL_HANDLE) {
            return;
        }
        timestamp_pending_[static_cast<std::size_t>(frame_index % timestamp_slot_count)] = true;
    }

    [[nodiscard]] std::uint64_t timestamp_delta(std::uint64_t start,
                                                std::uint64_t end) const noexcept {
        if (timestamp_valid_bits_ >= 64) {
            return end - start;
        }
        const auto mask = (std::uint64_t{1} << timestamp_valid_bits_) - 1;
        return (end - start) & mask;
    }

    [[nodiscard]] double timestamp_milliseconds(std::uint64_t start,
                                                std::uint64_t end) const noexcept {
        const auto nanoseconds = static_cast<double>(timestamp_delta(start, end)) *
                                 static_cast<double>(properties_.limits.timestampPeriod);
        return nanoseconds / 1'000'000.0;
    }

    [[nodiscard]] VulkanGpuTimingSample collect_latest_gpu_timing() noexcept {
        VulkanGpuTimingSample newest;
        if (timestamp_query_pool_ == VK_NULL_HANDLE) {
            return newest;
        }
        for (std::size_t slot = 0; slot < timestamp_pending_.size(); ++slot) {
            if (!timestamp_pending_[slot]) {
                continue;
            }
            std::array<std::uint64_t, timestamps_per_slot> values{};
            const auto result = vkGetQueryPoolResults(
                device_, timestamp_query_pool_,
                static_cast<std::uint32_t>(slot) * timestamps_per_slot, timestamps_per_slot,
                sizeof(values), values.data(), sizeof(std::uint64_t), VK_QUERY_RESULT_64_BIT);
            if (result != VK_SUCCESS) {
                continue;
            }
            timestamp_pending_[slot] = false;
            VulkanGpuTimingSample sample;
            sample.valid = true;
            sample.frame_index = timestamp_frame_indices_[slot];
            sample.frame_ms =
                timestamp_milliseconds(values[frame_start_timestamp], values[frame_end_timestamp]);
            sample.opaque_ms = timestamp_milliseconds(values[opaque_start_timestamp],
                                                      values[opaque_end_timestamp]);
            sample.alpha_tested_ms = timestamp_milliseconds(values[alpha_tested_start_timestamp],
                                                            values[alpha_tested_end_timestamp]);
            sample.transparent_ms = timestamp_milliseconds(values[transparent_start_timestamp],
                                                           values[transparent_end_timestamp]);
            sample.transfer_ms = timestamp_milliseconds(values[transfer_start_timestamp],
                                                        values[transfer_end_timestamp]);
            sample.final_copy_ms = timestamp_milliseconds(values[final_copy_start_timestamp],
                                                          values[final_copy_end_timestamp]);
            if (!newest.valid || sample.frame_index > newest.frame_index) {
                newest = sample;
            }
        }
        return newest;
    }

    static void attach_gpu_timing(rhi::RenderFrameStats& stats, const VulkanGpuTimingSample& timing,
                                  std::uint64_t current_frame) noexcept {
        if (!timing.valid) {
            return;
        }
        stats.gpu_timing_valid = true;
        stats.gpu_timing_frame_index = timing.frame_index;
        const auto latency = current_frame >= timing.frame_index
                                 ? current_frame - timing.frame_index
                                 : std::uint64_t{0};
        stats.gpu_timing_latency_frames = static_cast<std::uint32_t>(std::min(
            latency, static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max())));
        stats.gpu_frame_ms = timing.frame_ms;
        stats.gpu_opaque_terrain_ms = timing.opaque_ms;
        stats.gpu_alpha_tested_terrain_ms = timing.alpha_tested_ms;
        stats.gpu_transparent_terrain_ms = timing.transparent_ms;
        stats.gpu_transfer_ms = timing.transfer_ms;
        stats.gpu_final_copy_ms = timing.final_copy_ms;
    }

    [[nodiscard]] VulkanGraphicsPipelineResource*
    find_graphics_pipeline_for_material(const core::PrototypeId& material_id) noexcept {
        const auto found =
            std::ranges::find_if(graphics_pipelines_, [&material_id](const auto& pipeline) {
                return pipeline.second.desc.material_id == material_id;
            });
        return found == graphics_pipelines_.end() ? nullptr : &found->second;
    }

    void destroy_buffer_resource(VulkanBufferResource& resource) noexcept {
        if (resource.buffer != VK_NULL_HANDLE) {
            vkDestroyBuffer(device_, resource.buffer, nullptr);
            resource.buffer = VK_NULL_HANDLE;
        }
        if (resource.memory != VK_NULL_HANDLE) {
            vkFreeMemory(device_, resource.memory, nullptr);
            resource.memory = VK_NULL_HANDLE;
        }
        resource.byte_size = 0;
    }

    void destroy_buffer_resources() noexcept {
        for (auto& [_, resource] : buffer_resources_) {
            destroy_buffer_resource(resource);
        }
        buffer_resources_.clear();
    }

    [[nodiscard]] core::Status
    create_staging_buffer(std::size_t byte_size, VulkanBufferResource& resource, void*& mapped) {
        resource.desc = {rhi::RenderBufferUsage::storage, byte_size, "persistent_staging",
                         rhi::RenderBufferMemory::host_visible};
        resource.byte_size = byte_size;
        VkBufferCreateInfo buffer_info{};
        buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        buffer_info.size = static_cast<VkDeviceSize>(byte_size);
        buffer_info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        auto result = vkCreateBuffer(device_, &buffer_info, nullptr, &resource.buffer);
        if (result != VK_SUCCESS) {
            return core::Status::failure("renderer.vulkan_staging_buffer_failed",
                                         "failed to create Vulkan staging buffer: " +
                                             std::string(vk_result_name(result)));
        }
        VkMemoryRequirements requirements{};
        vkGetBufferMemoryRequirements(device_, resource.buffer, &requirements);
        auto memory_type = find_required_memory_type(physical_device_, requirements.memoryTypeBits,
                                                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                         VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                                                     "persistent staging buffer");
        if (!memory_type) {
            destroy_buffer_resource(resource);
            return core::Status::failure(memory_type.error().code, memory_type.error().message);
        }
        VkMemoryAllocateInfo allocation{};
        allocation.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocation.allocationSize = requirements.size;
        allocation.memoryTypeIndex = memory_type.value();
        result = vkAllocateMemory(device_, &allocation, nullptr, &resource.memory);
        if (result == VK_SUCCESS) {
            result = vkBindBufferMemory(device_, resource.buffer, resource.memory, 0);
        }
        if (result == VK_SUCCESS) {
            result = vkMapMemory(device_, resource.memory, 0, static_cast<VkDeviceSize>(byte_size),
                                 0, &mapped);
        }
        if (result != VK_SUCCESS) {
            destroy_buffer_resource(resource);
            mapped = nullptr;
            return core::Status::failure("renderer.vulkan_staging_memory_failed",
                                         "failed to allocate or map Vulkan staging memory: " +
                                             std::string(vk_result_name(result)));
        }
        return core::Status::ok();
    }

    [[nodiscard]] core::Status ensure_staging_buffer(std::size_t byte_size) {
        if (staging_buffer_.buffer != VK_NULL_HANDLE && staging_buffer_.byte_size >= byte_size) {
            return core::Status::ok();
        }
        destroy_staging_buffer();
        return create_staging_buffer(byte_size, staging_buffer_, staging_mapped_);
    }

    void destroy_staging_buffer() noexcept {
        if (staging_mapped_ != nullptr && staging_buffer_.memory != VK_NULL_HANDLE) {
            vkUnmapMemory(device_, staging_buffer_.memory);
            staging_mapped_ = nullptr;
        }
        destroy_buffer_resource(staging_buffer_);
    }

    [[nodiscard]] core::Status ensure_default_sampler() {
        if (default_sampler_ != VK_NULL_HANDLE) {
            return core::Status::ok();
        }
        VkSamplerCreateInfo sampler_info{};
        sampler_info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        sampler_info.magFilter = VK_FILTER_NEAREST;
        sampler_info.minFilter = VK_FILTER_NEAREST;
        sampler_info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        sampler_info.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        sampler_info.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        sampler_info.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        sampler_info.maxAnisotropy = 1.0F;
        sampler_info.minLod = 0.0F;
        sampler_info.maxLod = VK_LOD_CLAMP_NONE;
        const auto result = vkCreateSampler(device_, &sampler_info, nullptr, &default_sampler_);
        if (result != VK_SUCCESS) {
            default_sampler_ = VK_NULL_HANDLE;
            return core::Status::failure("renderer.vulkan_sampler_failed",
                                         "failed to create Vulkan default sampler: " +
                                             std::string(vk_result_name(result)));
        }
        return core::Status::ok();
    }

    void destroy_image_resource(VulkanImageResource& resource) noexcept {
        if (resource.owns_sampler && resource.sampler != VK_NULL_HANDLE) {
            vkDestroySampler(device_, resource.sampler, nullptr);
        }
        resource.sampler = VK_NULL_HANDLE;
        resource.owns_sampler = false;
        if (resource.image_view != VK_NULL_HANDLE) {
            vkDestroyImageView(device_, resource.image_view, nullptr);
            resource.image_view = VK_NULL_HANDLE;
        }
        if (resource.image != VK_NULL_HANDLE) {
            vkDestroyImage(device_, resource.image, nullptr);
            resource.image = VK_NULL_HANDLE;
        }
        if (resource.memory != VK_NULL_HANDLE) {
            vkFreeMemory(device_, resource.memory, nullptr);
            resource.memory = VK_NULL_HANDLE;
        }
        resource.layout = VK_IMAGE_LAYOUT_UNDEFINED;
        resource.byte_size = 0;
    }

    void destroy_image_resources() noexcept {
        for (auto& [_, resource] : image_resources_) {
            destroy_image_resource(resource);
        }
        image_resources_.clear();
    }

    void destroy_sampler_resource(VulkanSamplerResource& resource) noexcept {
        if (resource.sampler != VK_NULL_HANDLE) {
            vkDestroySampler(device_, resource.sampler, nullptr);
            resource.sampler = VK_NULL_HANDLE;
        }
    }

    void destroy_sampler_resources() noexcept {
        for (auto& [_, resource] : sampler_resources_) {
            destroy_sampler_resource(resource);
        }
        sampler_resources_.clear();
    }

    void destroy_frame_image_resource(VulkanFrameImageResource& resource) noexcept {
        if (resource.image != VK_NULL_HANDLE) {
            vkDestroyImage(device_, resource.image, nullptr);
            resource.image = VK_NULL_HANDLE;
        }
        if (resource.memory != VK_NULL_HANDLE) {
            vkFreeMemory(device_, resource.memory, nullptr);
            resource.memory = VK_NULL_HANDLE;
        }
        resource.layout = VK_IMAGE_LAYOUT_UNDEFINED;
    }

    void destroy_frame_image_resources(std::vector<VulkanFrameImageResource>& resources) noexcept {
        for (auto& resource : resources) {
            destroy_frame_image_resource(resource);
        }
        resources.clear();
    }

    [[nodiscard]] core::Result<VulkanFrameImageResource>
    create_frame_image_resource(std::string name, rhi::RenderExtent extent) {
        VulkanFrameImageResource resource;
        resource.name = std::move(name);

        VkImageCreateInfo image_info{};
        image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        image_info.imageType = VK_IMAGE_TYPE_2D;
        image_info.format = VK_FORMAT_R8G8B8A8_UNORM;
        image_info.extent = VkExtent3D{extent.width, extent.height, 1};
        image_info.mipLevels = 1;
        image_info.arrayLayers = 1;
        image_info.samples = VK_SAMPLE_COUNT_1_BIT;
        image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
        image_info.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                           VK_IMAGE_USAGE_SAMPLED_BIT;
        image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

        auto result = vkCreateImage(device_, &image_info, nullptr, &resource.image);
        if (result != VK_SUCCESS) {
            return core::Result<VulkanFrameImageResource>::failure(
                "renderer.vulkan_frame_image_failed",
                "failed to create Vulkan frame image: " + std::string(vk_result_name(result)));
        }

        VkMemoryRequirements requirements{};
        vkGetImageMemoryRequirements(device_, resource.image, &requirements);
        auto memory_type = find_memory_type(physical_device_, requirements.memoryTypeBits,
                                            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (!memory_type) {
            destroy_frame_image_resource(resource);
            return core::Result<VulkanFrameImageResource>::failure(memory_type.error().code,
                                                                   memory_type.error().message);
        }

        VkMemoryAllocateInfo allocation_info{};
        allocation_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocation_info.allocationSize = requirements.size;
        allocation_info.memoryTypeIndex = memory_type.value();
        result = vkAllocateMemory(device_, &allocation_info, nullptr, &resource.memory);
        if (result != VK_SUCCESS) {
            destroy_frame_image_resource(resource);
            return core::Result<VulkanFrameImageResource>::failure(
                "renderer.vulkan_frame_image_memory_failed",
                "failed to allocate Vulkan frame image memory: " +
                    std::string(vk_result_name(result)));
        }

        result = vkBindImageMemory(device_, resource.image, resource.memory, 0);
        if (result != VK_SUCCESS) {
            destroy_frame_image_resource(resource);
            return core::Result<VulkanFrameImageResource>::failure(
                "renderer.vulkan_frame_image_bind_failed",
                "failed to bind Vulkan frame image memory: " + std::string(vk_result_name(result)));
        }

        return core::Result<VulkanFrameImageResource>::success(std::move(resource));
    }

    void destroy_shader_module_resource(VulkanShaderModuleResource& resource) noexcept {
        if (resource.shader_module != VK_NULL_HANDLE) {
            vkDestroyShaderModule(device_, resource.shader_module, nullptr);
            resource.shader_module = VK_NULL_HANDLE;
        }
        resource.word_count = 0;
    }

    void destroy_shader_module_resources() noexcept {
        for (auto& [_, resource] : shader_modules_) {
            destroy_shader_module_resource(resource);
        }
        shader_modules_.clear();
    }

    void destroy_compute_pipeline_resource(VulkanComputePipelineResource& resource) noexcept {
        if (resource.pipeline != VK_NULL_HANDLE) {
            vkDestroyPipeline(device_, resource.pipeline, nullptr);
            resource.pipeline = VK_NULL_HANDLE;
        }
    }

    void destroy_compute_pipeline_resources() noexcept {
        for (auto& [_, resource] : compute_pipelines_) {
            destroy_compute_pipeline_resource(resource);
        }
        compute_pipelines_.clear();
    }

    void destroy_graphics_pipeline_resource(VulkanGraphicsPipelineResource& resource) noexcept {
        if (resource.pipeline != VK_NULL_HANDLE) {
            vkDestroyPipeline(device_, resource.pipeline, nullptr);
            resource.pipeline = VK_NULL_HANDLE;
        }
    }

    void destroy_graphics_pipeline_resources() noexcept {
        for (auto& [_, resource] : graphics_pipelines_) {
            destroy_graphics_pipeline_resource(resource);
        }
        graphics_pipelines_.clear();
    }

    void retire_compute_pipelines_for_shader(rhi::RenderResourceHandle shader) {
        for (auto pipeline = compute_pipelines_.begin(); pipeline != compute_pipelines_.end();) {
            if (pipeline->second.desc.compute_shader.value == shader.value) {
                retired_compute_pipelines_.push_back(
                    {std::move(pipeline->second), last_submission_serial_});
                pipeline = compute_pipelines_.erase(pipeline);
            } else {
                ++pipeline;
            }
        }
    }

    void retire_graphics_pipelines_for_shader(rhi::RenderResourceHandle shader) {
        for (auto pipeline = graphics_pipelines_.begin(); pipeline != graphics_pipelines_.end();) {
            if (pipeline->second.desc.vertex_shader.value == shader.value ||
                pipeline->second.desc.fragment_shader.value == shader.value) {
                retired_graphics_pipelines_.push_back(
                    {std::move(pipeline->second), last_submission_serial_});
                pipeline = graphics_pipelines_.erase(pipeline);
            } else {
                ++pipeline;
            }
        }
    }

    void destroy_compute_pipelines_for_shader(rhi::RenderResourceHandle shader) noexcept {
        for (auto pipeline = compute_pipelines_.begin(); pipeline != compute_pipelines_.end();) {
            if (pipeline->second.desc.compute_shader.value == shader.value) {
                destroy_compute_pipeline_resource(pipeline->second);
                pipeline = compute_pipelines_.erase(pipeline);
            } else {
                ++pipeline;
            }
        }
    }

    void destroy_graphics_pipelines_for_shader(rhi::RenderResourceHandle shader) noexcept {
        for (auto pipeline = graphics_pipelines_.begin(); pipeline != graphics_pipelines_.end();) {
            if (pipeline->second.desc.vertex_shader.value == shader.value ||
                pipeline->second.desc.fragment_shader.value == shader.value) {
                destroy_graphics_pipeline_resource(pipeline->second);
                pipeline = graphics_pipelines_.erase(pipeline);
            } else {
                ++pipeline;
            }
        }
    }

    void destroy_compute_pipelines_for_material(const core::PrototypeId& material_id) noexcept {
        for (auto pipeline = compute_pipelines_.begin(); pipeline != compute_pipelines_.end();) {
            if (pipeline->second.desc.material_id == material_id) {
                destroy_compute_pipeline_resource(pipeline->second);
                pipeline = compute_pipelines_.erase(pipeline);
            } else {
                ++pipeline;
            }
        }
    }

    void destroy_graphics_pipelines_for_material(const core::PrototypeId& material_id) noexcept {
        for (auto pipeline = graphics_pipelines_.begin(); pipeline != graphics_pipelines_.end();) {
            if (pipeline->second.desc.material_id == material_id) {
                destroy_graphics_pipeline_resource(pipeline->second);
                pipeline = graphics_pipelines_.erase(pipeline);
            } else {
                ++pipeline;
            }
        }
    }

    void destroy_descriptor_writes_for_resource(rhi::RenderResourceHandle resource) noexcept {
        for (auto write = descriptor_write_records_.begin();
             write != descriptor_write_records_.end();) {
            if (write->second.resource.value == resource.value ||
                write->second.sampler.value == resource.value) {
                write = descriptor_write_records_.erase(write);
            } else {
                ++write;
            }
        }
    }

    void destroy_descriptor_writes_for_material(const core::PrototypeId& material_id) noexcept {
        for (auto write = descriptor_write_records_.begin();
             write != descriptor_write_records_.end();) {
            if (write->second.material_id == material_id) {
                write = descriptor_write_records_.erase(write);
            } else {
                ++write;
            }
        }
    }

    void destroy_pipeline_layout_resource(VulkanPipelineLayoutResource& resource) noexcept {
        if (resource.descriptor_pool != VK_NULL_HANDLE) {
            vkDestroyDescriptorPool(device_, resource.descriptor_pool, nullptr);
            resource.descriptor_pool = VK_NULL_HANDLE;
            resource.descriptor_set = VK_NULL_HANDLE;
        }
        if (resource.pipeline_layout != VK_NULL_HANDLE) {
            vkDestroyPipelineLayout(device_, resource.pipeline_layout, nullptr);
            resource.pipeline_layout = VK_NULL_HANDLE;
        }
        if (resource.descriptor_set_layout != VK_NULL_HANDLE) {
            vkDestroyDescriptorSetLayout(device_, resource.descriptor_set_layout, nullptr);
            resource.descriptor_set_layout = VK_NULL_HANDLE;
        }
    }

    void destroy_pipeline_layout_resources() noexcept {
        for (auto& [_, resource] : pipeline_layouts_) {
            destroy_pipeline_layout_resource(resource);
        }
        pipeline_layouts_.clear();
    }

    [[nodiscard]] core::Result<VulkanPipelineLayoutResource>
    create_pipeline_layout_resource(rhi::RenderPipelineLayoutDesc desc) {
        VulkanPipelineLayoutResource resource;
        resource.desc = std::move(desc);

        std::vector<VkDescriptorSetLayoutBinding> bindings;
        bindings.reserve(resource.desc.descriptors.size());
        for (const auto& descriptor : resource.desc.descriptors) {
            VkDescriptorSetLayoutBinding binding{};
            binding.binding = descriptor.slot;
            binding.descriptorType = vulkan_descriptor_type(descriptor.kind);
            binding.descriptorCount = 1;
            binding.stageFlags = vulkan_shader_stage_flags(descriptor.stages);
            bindings.push_back(binding);
        }

        VkDescriptorSetLayoutCreateInfo set_layout_info{};
        set_layout_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        set_layout_info.bindingCount = static_cast<std::uint32_t>(bindings.size());
        set_layout_info.pBindings = bindings.empty() ? nullptr : bindings.data();
        auto result = vkCreateDescriptorSetLayout(device_, &set_layout_info, nullptr,
                                                  &resource.descriptor_set_layout);
        if (result != VK_SUCCESS) {
            return core::Result<VulkanPipelineLayoutResource>::failure(
                "renderer.vulkan_descriptor_set_layout_failed",
                "failed to create Vulkan descriptor set layout: " +
                    std::string(vk_result_name(result)));
        }

        VkPipelineLayoutCreateInfo pipeline_layout_info{};
        pipeline_layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        pipeline_layout_info.setLayoutCount = 1;
        pipeline_layout_info.pSetLayouts = &resource.descriptor_set_layout;
        std::vector<VkPushConstantRange> push_constant_ranges;
        push_constant_ranges.reserve(resource.desc.push_constant_ranges.size());
        for (const auto& range : resource.desc.push_constant_ranges) {
            if (range.byte_offset > properties_.limits.maxPushConstantsSize ||
                range.byte_size > properties_.limits.maxPushConstantsSize - range.byte_offset) {
                destroy_pipeline_layout_resource(resource);
                return core::Result<VulkanPipelineLayoutResource>::failure(
                    "renderer.push_constants_exceed_device_limit",
                    "pipeline layout push constants exceed Vulkan device limit of " +
                        std::to_string(properties_.limits.maxPushConstantsSize) + " bytes");
            }
            push_constant_ranges.push_back(VkPushConstantRange{
                vulkan_shader_stage_flags(range.stages), range.byte_offset, range.byte_size});
        }
        pipeline_layout_info.pushConstantRangeCount =
            static_cast<std::uint32_t>(push_constant_ranges.size());
        pipeline_layout_info.pPushConstantRanges =
            push_constant_ranges.empty() ? nullptr : push_constant_ranges.data();
        result = vkCreatePipelineLayout(device_, &pipeline_layout_info, nullptr,
                                        &resource.pipeline_layout);
        if (result != VK_SUCCESS) {
            destroy_pipeline_layout_resource(resource);
            return core::Result<VulkanPipelineLayoutResource>::failure(
                "renderer.vulkan_pipeline_layout_failed",
                "failed to create Vulkan pipeline layout: " + std::string(vk_result_name(result)));
        }

        auto pool_sizes = make_descriptor_pool_sizes(resource.desc);
        // A layout that samples frame graph resources needs one set per frame in flight, so the
        // pool has to be sized for all of them.
        const auto set_count =
            resource.desc.per_frame_descriptors && !frame_contexts_.empty()
                ? static_cast<std::uint32_t>(frame_contexts_.size())
                : 1U;
        if (!pool_sizes.empty()) {
            for (auto& pool_size : pool_sizes) {
                pool_size.descriptorCount *= set_count;
            }
            VkDescriptorPoolCreateInfo pool_info{};
            pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
            pool_info.maxSets = set_count;
            pool_info.poolSizeCount = static_cast<std::uint32_t>(pool_sizes.size());
            pool_info.pPoolSizes = pool_sizes.data();
            result =
                vkCreateDescriptorPool(device_, &pool_info, nullptr, &resource.descriptor_pool);
            if (result != VK_SUCCESS) {
                destroy_pipeline_layout_resource(resource);
                return core::Result<VulkanPipelineLayoutResource>::failure(
                    "renderer.vulkan_descriptor_pool_failed",
                    "failed to create Vulkan descriptor pool: " +
                        std::string(vk_result_name(result)));
            }

            const std::vector<VkDescriptorSetLayout> set_layouts(set_count,
                                                                 resource.descriptor_set_layout);
            std::vector<VkDescriptorSet> allocated(set_count, VK_NULL_HANDLE);
            VkDescriptorSetAllocateInfo allocate_info{};
            allocate_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
            allocate_info.descriptorPool = resource.descriptor_pool;
            allocate_info.descriptorSetCount = set_count;
            allocate_info.pSetLayouts = set_layouts.data();
            result = vkAllocateDescriptorSets(device_, &allocate_info, allocated.data());
            if (result != VK_SUCCESS) {
                destroy_pipeline_layout_resource(resource);
                return core::Result<VulkanPipelineLayoutResource>::failure(
                    "renderer.vulkan_descriptor_set_failed",
                    "failed to allocate Vulkan descriptor set: " +
                        std::string(vk_result_name(result)));
            }
            // The first set stays the one non-graph descriptor writes target, so materials that do
            // not sample graph resources behave exactly as before.
            resource.descriptor_set = allocated.front();
            if (resource.desc.per_frame_descriptors) {
                resource.per_frame_descriptor_sets = std::move(allocated);
            }
        }

        return core::Result<VulkanPipelineLayoutResource>::success(std::move(resource));
    }

    void destroy_swapchain() noexcept {
        for (const auto semaphore : swapchain_render_finished_semaphores_) {
            if (semaphore != VK_NULL_HANDLE) {
                vkDestroySemaphore(device_, semaphore, nullptr);
            }
        }
        swapchain_render_finished_semaphores_.clear();
        if (swapchain_ != VK_NULL_HANDLE) {
            vkDestroySwapchainKHR(device_, swapchain_, nullptr);
            swapchain_ = VK_NULL_HANDLE;
        }
        swapchain_images_.clear();
        swapchain_image_layouts_.clear();
        swapchain_extent_ = {};
        swapchain_format_ = VK_FORMAT_UNDEFINED;
    }

    void destroy_offscreen_target() noexcept {
        if (depth_image_view_ != VK_NULL_HANDLE) {
            vkDestroyImageView(device_, depth_image_view_, nullptr);
            depth_image_view_ = VK_NULL_HANDLE;
        }
        if (depth_image_ != VK_NULL_HANDLE) {
            vkDestroyImage(device_, depth_image_, nullptr);
            depth_image_ = VK_NULL_HANDLE;
        }
        if (depth_memory_ != VK_NULL_HANDLE) {
            vkFreeMemory(device_, depth_memory_, nullptr);
            depth_memory_ = VK_NULL_HANDLE;
        }
        if (offscreen_image_view_ != VK_NULL_HANDLE) {
            vkDestroyImageView(device_, offscreen_image_view_, nullptr);
            offscreen_image_view_ = VK_NULL_HANDLE;
        }
        if (offscreen_image_ != VK_NULL_HANDLE) {
            vkDestroyImage(device_, offscreen_image_, nullptr);
            offscreen_image_ = VK_NULL_HANDLE;
        }
        if (offscreen_memory_ != VK_NULL_HANDLE) {
            vkFreeMemory(device_, offscreen_memory_, nullptr);
            offscreen_memory_ = VK_NULL_HANDLE;
        }
        offscreen_extent_ = {};
        offscreen_layout_ = VK_IMAGE_LAYOUT_UNDEFINED;
        depth_layout_ = VK_IMAGE_LAYOUT_UNDEFINED;
    }

    [[nodiscard]] core::Status upload_sampled_image(VulkanUploadContext& upload_context,
                                                    VkBuffer staging_buffer,
                                                    VkDeviceSize staging_offset,
                                                    VulkanImageResource& resource,
                                                    std::uint64_t upload_serial) {
        const auto commands = upload_context.transfer_commands;
        VkCommandBufferBeginInfo begin_info{};
        begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        auto result = vkBeginCommandBuffer(commands, &begin_info);
        if (result != VK_SUCCESS) {
            return core::Status::failure("renderer.vulkan_image_upload_begin_failed",
                                         "failed to begin Vulkan image upload commands: " +
                                             std::string(vk_result_name(result)));
        }
        if (upload_context.timestamp_queries != VK_NULL_HANDLE) {
            vkCmdResetQueryPool(commands, upload_context.timestamp_queries, 0, 2);
            vkCmdWriteTimestamp(commands, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                upload_context.timestamp_queries, 0);
        }
        begin_debug_label(commands, "Sampled image upload", 0.92F, 0.56F, 0.16F);

        const VkImageSubresourceRange image_range{
            VK_IMAGE_ASPECT_COLOR_BIT, 0, resource.desc.mip_levels, 0, resource.desc.array_layers,
        };

        VkImageMemoryBarrier to_transfer{};
        to_transfer.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        to_transfer.srcAccessMask = 0;
        to_transfer.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        to_transfer.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        to_transfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        to_transfer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        to_transfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        to_transfer.image = resource.image;
        to_transfer.subresourceRange = image_range;
        vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                             &to_transfer);

        std::vector<VkBufferImageCopy> copy_regions;
        copy_regions.reserve(resource.desc.mip_levels);
        VkDeviceSize buffer_offset = staging_offset;
        auto mip_width = resource.desc.width;
        auto mip_height = resource.desc.height;
        const auto bytes_per_pixel = rhi::render_image_format_bytes_per_pixel(resource.desc.format);
        for (std::uint32_t mip = 0; mip < resource.desc.mip_levels; ++mip) {
            VkBufferImageCopy copy_region{};
            copy_region.bufferOffset = buffer_offset;
            copy_region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            copy_region.imageSubresource.mipLevel = mip;
            copy_region.imageSubresource.baseArrayLayer = 0;
            copy_region.imageSubresource.layerCount = resource.desc.array_layers;
            copy_region.imageExtent = VkExtent3D{mip_width, mip_height, 1};
            copy_regions.push_back(copy_region);
            buffer_offset += static_cast<VkDeviceSize>(mip_width) * mip_height *
                             resource.desc.array_layers * bytes_per_pixel;
            mip_width = std::max(1U, mip_width / 2U);
            mip_height = std::max(1U, mip_height / 2U);
        }
        vkCmdCopyBufferToImage(
            commands, staging_buffer, resource.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            static_cast<std::uint32_t>(copy_regions.size()), copy_regions.data());

        VkImageMemoryBarrier to_shader_read{};
        to_shader_read.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        to_shader_read.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        to_shader_read.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        to_shader_read.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        to_shader_read.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        to_shader_read.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        to_shader_read.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        to_shader_read.image = resource.image;
        to_shader_read.subresourceRange = image_range;
        vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_VERTEX_SHADER_BIT |
                                 VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                             0, 0, nullptr, 0, nullptr, 1, &to_shader_read);
        if (upload_context.timestamp_queries != VK_NULL_HANDLE) {
            vkCmdWriteTimestamp(commands, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                                upload_context.timestamp_queries, 1);
        }
        end_debug_label(commands);

        result = vkEndCommandBuffer(commands);
        if (result != VK_SUCCESS) {
            return core::Status::failure("renderer.vulkan_image_upload_end_failed",
                                         "failed to end Vulkan image upload commands: " +
                                             std::string(vk_result_name(result)));
        }

        VkSubmitInfo submit_info{};
        submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit_info.commandBufferCount = 1;
        submit_info.pCommandBuffers = &commands;
        result = vkQueueSubmit(queue_, 1, &submit_info, upload_context.completion_fence);
        if (result != VK_SUCCESS) {
            return core::Status::failure("renderer.vulkan_queue_submit_failed",
                                         "failed to submit Vulkan sampled image upload: " +
                                             std::string(vk_result_name(result)));
        }

        last_submission_serial_ = upload_serial;
        upload_context.submission_serial = upload_serial;
        upload_context.in_flight = true;
        upload_context.timing_pending = upload_context.timestamp_queries != VK_NULL_HANDLE;
        resource.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        return core::Status::ok();
    }

    [[nodiscard]] core::Status ensure_swapchain() {
        if (swapchain_ != VK_NULL_HANDLE) {
            return core::Status::ok();
        }
        if (surface_ == VK_NULL_HANDLE) {
            return core::Status::failure("renderer.vulkan_present_unavailable",
                                         "Vulkan smoke device has no window surface");
        }

        auto support = query_swapchain_support(physical_device_, surface_);
        if (!support) {
            return core::Status::failure(support.error().code, support.error().message);
        }
        if (support.value().formats.empty() || support.value().present_modes.empty()) {
            return core::Status::failure(
                "renderer.vulkan_swapchain_unavailable",
                "Vulkan surface does not expose swapchain formats or present modes");
        }
        if ((support.value().capabilities.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT) ==
            0) {
            return core::Status::failure(
                "renderer.vulkan_swapchain_clear_unavailable",
                "Vulkan surface images cannot be used as transfer clear targets");
        }

        const auto surface_format = choose_surface_format(support.value().formats);
        const auto present_mode =
            choose_present_mode(support.value().present_modes, desc_.present_mode);
        const auto extent =
            choose_swapchain_extent(support.value().capabilities, desc_.initial_extent);
        if (extent.width == 0 || extent.height == 0) {
            return core::Status::failure("renderer.vulkan_surface_extent_unavailable",
                                         "Vulkan surface extent is empty");
        }

        auto image_count = support.value().capabilities.minImageCount + 1;
        if (support.value().capabilities.maxImageCount > 0) {
            image_count = std::min(image_count, support.value().capabilities.maxImageCount);
        }

        VkCompositeAlphaFlagBitsKHR composite_alpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
        constexpr std::array<VkCompositeAlphaFlagBitsKHR, 4> composite_alpha_candidates{
            VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
            VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR,
            VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR,
            VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR,
        };
        for (const auto candidate : composite_alpha_candidates) {
            if ((support.value().capabilities.supportedCompositeAlpha & candidate) != 0) {
                composite_alpha = candidate;
                break;
            }
        }

        VkSwapchainCreateInfoKHR swapchain_info{};
        swapchain_info.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
        swapchain_info.surface = surface_;
        swapchain_info.minImageCount = image_count;
        swapchain_info.imageFormat = surface_format.format;
        swapchain_info.imageColorSpace = surface_format.colorSpace;
        swapchain_info.imageExtent = extent;
        swapchain_info.imageArrayLayers = 1;
        swapchain_info.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        swapchain_info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
        swapchain_info.preTransform = support.value().capabilities.currentTransform;
        swapchain_info.compositeAlpha = composite_alpha;
        swapchain_info.presentMode = present_mode;
        swapchain_info.clipped = VK_TRUE;
        swapchain_info.oldSwapchain = VK_NULL_HANDLE;

        const auto result = vkCreateSwapchainKHR(device_, &swapchain_info, nullptr, &swapchain_);
        if (result != VK_SUCCESS) {
            swapchain_ = VK_NULL_HANDLE;
            return core::Status::failure("renderer.vulkan_swapchain_failed",
                                         "failed to create Vulkan swapchain: " +
                                             std::string(vk_result_name(result)));
        }

        std::uint32_t swapchain_image_count = 0;
        auto image_result =
            vkGetSwapchainImagesKHR(device_, swapchain_, &swapchain_image_count, nullptr);
        if (image_result != VK_SUCCESS || swapchain_image_count == 0) {
            destroy_swapchain();
            return core::Status::failure("renderer.vulkan_swapchain_images_failed",
                                         "failed to query Vulkan swapchain images: " +
                                             std::string(vk_result_name(image_result)));
        }

        swapchain_images_.resize(swapchain_image_count);
        image_result = vkGetSwapchainImagesKHR(device_, swapchain_, &swapchain_image_count,
                                               swapchain_images_.data());
        if (image_result != VK_SUCCESS && image_result != VK_INCOMPLETE) {
            destroy_swapchain();
            return core::Status::failure("renderer.vulkan_swapchain_images_failed",
                                         "failed to query Vulkan swapchain images: " +
                                             std::string(vk_result_name(image_result)));
        }
        swapchain_images_.resize(swapchain_image_count);
        swapchain_image_layouts_.assign(swapchain_images_.size(), VK_IMAGE_LAYOUT_UNDEFINED);
        swapchain_render_finished_semaphores_.reserve(swapchain_images_.size());
        for (std::size_t index = 0; index < swapchain_images_.size(); ++index) {
            auto semaphore = create_semaphore(device_);
            if (!semaphore) {
                const auto error = semaphore.error();
                destroy_swapchain();
                return core::Status::failure(error.code, error.message);
            }
            swapchain_render_finished_semaphores_.push_back(semaphore.value());
        }
        swapchain_extent_ = rhi::RenderExtent{extent.width, extent.height};
        swapchain_format_ = surface_format.format;
        desc_.initial_extent = swapchain_extent_;
        return core::Status::ok();
    }

    [[nodiscard]] core::Status ensure_offscreen_target(rhi::RenderExtent extent) {
        if (offscreen_image_ != VK_NULL_HANDLE && depth_image_ != VK_NULL_HANDLE &&
            offscreen_extent_.width == extent.width && offscreen_extent_.height == extent.height) {
            return core::Status::ok();
        }

        destroy_offscreen_target();

        VkImageCreateInfo image_info{};
        image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        image_info.imageType = VK_IMAGE_TYPE_2D;
        image_info.format = VK_FORMAT_R8G8B8A8_UNORM;
        image_info.extent = VkExtent3D{extent.width, extent.height, 1};
        image_info.mipLevels = 1;
        image_info.arrayLayers = 1;
        image_info.samples = VK_SAMPLE_COUNT_1_BIT;
        image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
        image_info.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                           VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

        const auto image_result = vkCreateImage(device_, &image_info, nullptr, &offscreen_image_);
        if (image_result != VK_SUCCESS) {
            offscreen_image_ = VK_NULL_HANDLE;
            return core::Status::failure("renderer.vulkan_image_failed",
                                         "failed to create Vulkan offscreen image: " +
                                             std::string(vk_result_name(image_result)));
        }

        VkMemoryRequirements requirements{};
        vkGetImageMemoryRequirements(device_, offscreen_image_, &requirements);
        auto memory_type = find_memory_type(physical_device_, requirements.memoryTypeBits,
                                            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (!memory_type) {
            destroy_offscreen_target();
            return core::Status::failure(memory_type.error().code, memory_type.error().message);
        }

        VkMemoryAllocateInfo allocation_info{};
        allocation_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocation_info.allocationSize = requirements.size;
        allocation_info.memoryTypeIndex = memory_type.value();

        const auto memory_result =
            vkAllocateMemory(device_, &allocation_info, nullptr, &offscreen_memory_);
        if (memory_result != VK_SUCCESS) {
            destroy_offscreen_target();
            return core::Status::failure("renderer.vulkan_image_memory_failed",
                                         "failed to allocate Vulkan offscreen image memory: " +
                                             std::string(vk_result_name(memory_result)));
        }

        const auto bind_result = vkBindImageMemory(device_, offscreen_image_, offscreen_memory_, 0);
        if (bind_result != VK_SUCCESS) {
            destroy_offscreen_target();
            return core::Status::failure("renderer.vulkan_image_bind_failed",
                                         "failed to bind Vulkan offscreen image memory: " +
                                             std::string(vk_result_name(bind_result)));
        }

        offscreen_extent_ = extent;
        offscreen_layout_ = VK_IMAGE_LAYOUT_UNDEFINED;

        VkImageViewCreateInfo view_info{};
        view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        view_info.image = offscreen_image_;
        view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view_info.format = VK_FORMAT_R8G8B8A8_UNORM;
        view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        view_info.subresourceRange.baseMipLevel = 0;
        view_info.subresourceRange.levelCount = 1;
        view_info.subresourceRange.baseArrayLayer = 0;
        view_info.subresourceRange.layerCount = 1;
        const auto view_result =
            vkCreateImageView(device_, &view_info, nullptr, &offscreen_image_view_);
        if (view_result != VK_SUCCESS) {
            destroy_offscreen_target();
            return core::Status::failure("renderer.vulkan_image_view_failed",
                                         "failed to create Vulkan offscreen image view: " +
                                             std::string(vk_result_name(view_result)));
        }

        VkImageCreateInfo depth_image_info{};
        depth_image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        depth_image_info.imageType = VK_IMAGE_TYPE_2D;
        depth_image_info.format = depth_format_;
        depth_image_info.extent = VkExtent3D{extent.width, extent.height, 1};
        depth_image_info.mipLevels = 1;
        depth_image_info.arrayLayers = 1;
        depth_image_info.samples = VK_SAMPLE_COUNT_1_BIT;
        depth_image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
        depth_image_info.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
        depth_image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        depth_image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        const auto depth_image_result =
            vkCreateImage(device_, &depth_image_info, nullptr, &depth_image_);
        if (depth_image_result != VK_SUCCESS) {
            destroy_offscreen_target();
            return core::Status::failure("renderer.vulkan_depth_image_failed",
                                         "failed to create Vulkan depth image: " +
                                             std::string(vk_result_name(depth_image_result)));
        }

        VkMemoryRequirements depth_requirements{};
        vkGetImageMemoryRequirements(device_, depth_image_, &depth_requirements);
        auto depth_memory_type =
            find_memory_type(physical_device_, depth_requirements.memoryTypeBits,
                             VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (!depth_memory_type) {
            destroy_offscreen_target();
            return core::Status::failure(depth_memory_type.error().code,
                                         depth_memory_type.error().message);
        }

        VkMemoryAllocateInfo depth_allocation_info{};
        depth_allocation_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        depth_allocation_info.allocationSize = depth_requirements.size;
        depth_allocation_info.memoryTypeIndex = depth_memory_type.value();
        const auto depth_memory_result =
            vkAllocateMemory(device_, &depth_allocation_info, nullptr, &depth_memory_);
        if (depth_memory_result != VK_SUCCESS) {
            destroy_offscreen_target();
            return core::Status::failure("renderer.vulkan_depth_memory_failed",
                                         "failed to allocate Vulkan depth memory: " +
                                             std::string(vk_result_name(depth_memory_result)));
        }

        const auto depth_bind_result = vkBindImageMemory(device_, depth_image_, depth_memory_, 0);
        if (depth_bind_result != VK_SUCCESS) {
            destroy_offscreen_target();
            return core::Status::failure("renderer.vulkan_depth_bind_failed",
                                         "failed to bind Vulkan depth image memory: " +
                                             std::string(vk_result_name(depth_bind_result)));
        }

        VkImageViewCreateInfo depth_view_info{};
        depth_view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        depth_view_info.image = depth_image_;
        depth_view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
        depth_view_info.format = depth_format_;
        depth_view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
        if (vulkan_format_has_stencil(depth_format_)) {
            depth_view_info.subresourceRange.aspectMask |= VK_IMAGE_ASPECT_STENCIL_BIT;
        }
        depth_view_info.subresourceRange.baseMipLevel = 0;
        depth_view_info.subresourceRange.levelCount = 1;
        depth_view_info.subresourceRange.baseArrayLayer = 0;
        depth_view_info.subresourceRange.layerCount = 1;
        const auto depth_view_result =
            vkCreateImageView(device_, &depth_view_info, nullptr, &depth_image_view_);
        if (depth_view_result != VK_SUCCESS) {
            destroy_offscreen_target();
            return core::Status::failure("renderer.vulkan_depth_view_failed",
                                         "failed to create Vulkan depth image view: " +
                                             std::string(vk_result_name(depth_view_result)));
        }
        depth_layout_ = VK_IMAGE_LAYOUT_UNDEFINED;
        return core::Status::ok();
    }

    [[nodiscard]] core::Status ensure_frame_target(VulkanFrameContext& context,
                                                   rhi::RenderExtent extent) {
        auto& target = context.target;
        if (target.color_image != VK_NULL_HANDLE && target.depth_image != VK_NULL_HANDLE &&
            target.extent.width == extent.width && target.extent.height == extent.height) {
            return core::Status::ok();
        }
        destroy_frame_target(target);
        auto status = ensure_offscreen_target(extent);
        if (!status) {
            return status;
        }
        target.color_image = std::exchange(offscreen_image_, VK_NULL_HANDLE);
        target.color_view = std::exchange(offscreen_image_view_, VK_NULL_HANDLE);
        target.color_memory = std::exchange(offscreen_memory_, VK_NULL_HANDLE);
        target.color_layout = std::exchange(offscreen_layout_, VK_IMAGE_LAYOUT_UNDEFINED);
        target.depth_image = std::exchange(depth_image_, VK_NULL_HANDLE);
        target.depth_view = std::exchange(depth_image_view_, VK_NULL_HANDLE);
        target.depth_memory = std::exchange(depth_memory_, VK_NULL_HANDLE);
        target.depth_layout = std::exchange(depth_layout_, VK_IMAGE_LAYOUT_UNDEFINED);
        target.extent = std::exchange(offscreen_extent_, rhi::RenderExtent{});
        return core::Status::ok();
    }

    [[nodiscard]] core::Result<rhi::RenderFrameStats>
    render_present_frame(rhi::RenderFrameDesc desc,
                         std::span<const VulkanFrameTransition> frame_transitions) {
        auto status = ensure_swapchain();
        if (!status) {
            return core::Result<rhi::RenderFrameStats>::failure(status.error().code,
                                                                status.error().message);
        }

        std::uint32_t image_index = 0;
        auto acquire_result =
            vkAcquireNextImageKHR(device_, swapchain_, std::numeric_limits<std::uint64_t>::max(),
                                  image_available_semaphore_, VK_NULL_HANDLE, &image_index);
        if (acquire_result == VK_ERROR_OUT_OF_DATE_KHR) {
            destroy_swapchain();
            status = ensure_swapchain();
            if (!status) {
                return core::Result<rhi::RenderFrameStats>::failure(status.error().code,
                                                                    status.error().message);
            }
            acquire_result = vkAcquireNextImageKHR(
                device_, swapchain_, std::numeric_limits<std::uint64_t>::max(),
                image_available_semaphore_, VK_NULL_HANDLE, &image_index);
        }
        if (acquire_result != VK_SUCCESS && acquire_result != VK_SUBOPTIMAL_KHR) {
            return core::Result<rhi::RenderFrameStats>::failure(
                "renderer.vulkan_acquire_image_failed",
                "failed to acquire Vulkan swapchain image: " +
                    std::string(vk_result_name(acquire_result)));
        }
        if (image_index >= swapchain_images_.size()) {
            return core::Result<rhi::RenderFrameStats>::failure(
                "renderer.vulkan_invalid_swapchain_image",
                "Vulkan acquired an out-of-range swapchain image");
        }

        status = submit_swapchain_clear(image_index, desc.clear_color, frame_transitions);
        if (!status) {
            return core::Result<rhi::RenderFrameStats>::failure(status.error().code,
                                                                status.error().message);
        }

        VkPresentInfoKHR present_info{};
        present_info.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
        present_info.waitSemaphoreCount = 1;
        present_info.pWaitSemaphores = &render_finished_semaphore_;
        present_info.swapchainCount = 1;
        present_info.pSwapchains = &swapchain_;
        present_info.pImageIndices = &image_index;

        const auto present_result = vkQueuePresentKHR(queue_, &present_info);
        const auto wait_result = vkWaitForFences(device_, 1, &fence_, VK_TRUE,
                                                 std::numeric_limits<std::uint64_t>::max());
        if (wait_result != VK_SUCCESS) {
            return core::Result<rhi::RenderFrameStats>::failure(
                "renderer.vulkan_wait_fence_failed", "failed to wait for Vulkan present fence: " +
                                                         std::string(vk_result_name(wait_result)));
        }
        if (present_result == VK_ERROR_OUT_OF_DATE_KHR) {
            destroy_swapchain();
            return core::Result<rhi::RenderFrameStats>::failure(
                "renderer.vulkan_present_out_of_date", "Vulkan swapchain is out of date");
        }
        if (present_result != VK_SUCCESS && present_result != VK_SUBOPTIMAL_KHR) {
            return core::Result<rhi::RenderFrameStats>::failure(
                "renderer.vulkan_present_failed", "failed to present Vulkan swapchain image: " +
                                                      std::string(vk_result_name(present_result)));
        }

        swapchain_image_layouts_[image_index] = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        rhi::RenderFrameStats stats;
        stats.backend = backend();
        stats.frame_index = next_frame_index_++;
        stats.submission_serial = ++last_submission_serial_;
        complete_all_submissions();
        advance_completed_submission(stats.submission_serial);
        stats.completed_submission_serial = completed_submission_serial_;
        stats.extent = swapchain_extent_;
        stats.clear_color = desc.clear_color;
        stats.presented = true;
        stats.render_pass_count = 2;
        stats.present_pass_count = 1;
        stats.synchronization_barrier_count = frame_transitions.size();
        stats.submitted_synchronization_barrier_count = frame_transitions.size();
        attach_latest_gpu_upload_timing(stats);
        ++completed_frame_count_;
        return core::Result<rhi::RenderFrameStats>::success(stats);
    }

    [[nodiscard]] core::Status
    validate_submitted_frame_resources(const rhi::RenderFrameSubmission& frame) const {
        if (frame.pass_commands.empty()) {
            return core::Status::ok();
        }
        for (const auto& pass_commands : frame.pass_commands) {
            const auto& pass = frame.plan.passes[pass_commands.pass_index];
            // Compatibility is a per-pass property, not a frame-wide one. Each pass binds its own
            // attachments, so a post-process pass drawing to the display image alongside world
            // passes drawing to an HDR target is correct and must not be rejected.
            const VulkanGraphicsPipelineResource* reference_pipeline = nullptr;
            for (const auto& draw : pass_commands.draws) {
                const auto pipeline = graphics_pipelines_.find(draw.pipeline.value);
                if (pipeline == graphics_pipelines_.end()) {
                    return core::Status::failure("renderer.unknown_graphics_pipeline",
                                                 "render draw references a graphics pipeline not "
                                                 "owned by this Vulkan device");
                }
                if (reference_pipeline == nullptr) {
                    reference_pipeline = &pipeline->second;
                } else if (pipeline->second.desc.color_target_format !=
                               reference_pipeline->desc.color_target_format ||
                           pipeline->second.uses_depth != reference_pipeline->uses_depth) {
                    return core::Status::failure(
                        "renderer.incompatible_graphics_pipelines",
                        "draw passes in one Vulkan frame require compatible color/depth targets");
                }

                // A non-indexed draw binds neither buffer; a fullscreen pass has no geometry.
                if (draw.vertex_buffer.is_valid()) {
                    const auto vertex = buffer_resources_.find(draw.vertex_buffer.value);
                    if (vertex == buffer_resources_.end()) {
                        return core::Status::failure(
                            "renderer.unknown_vertex_buffer",
                            "render draw references a vertex buffer not owned by this Vulkan "
                            "device");
                    }
                    if (vertex->second.desc.usage != rhi::RenderBufferUsage::vertex) {
                        return core::Status::failure(
                            "renderer.invalid_vertex_buffer_usage",
                            "render draw vertex buffer has non-vertex usage");
                    }
                }

                if (draw.index_buffer.is_valid()) {
                    const auto index = buffer_resources_.find(draw.index_buffer.value);
                    if (index == buffer_resources_.end()) {
                        return core::Status::failure(
                            "renderer.unknown_index_buffer",
                            "render draw references an index buffer not owned by this Vulkan "
                            "device");
                    }
                    if (index->second.desc.usage != rhi::RenderBufferUsage::index) {
                        return core::Status::failure("renderer.invalid_index_buffer_usage",
                                                     "render draw index buffer has non-index "
                                                     "usage");
                    }
                    const auto available_indices =
                        index->second.byte_size / rhi::render_index_type_size(draw.index_type);
                    const auto end_index = static_cast<std::size_t>(draw.first_index) +
                                           static_cast<std::size_t>(draw.index_count);
                    if (end_index > available_indices) {
                        return core::Status::failure(
                            "renderer.draw_index_range_out_of_bounds",
                            "render draw index range exceeds its index buffer");
                    }
                }

                const auto layout =
                    pipeline_layouts_.find(pipeline->second.desc.material_id.value());
                if (layout == pipeline_layouts_.end()) {
                    return core::Status::failure(
                        "renderer.unbound_graphics_pipeline_layout",
                        "render draw graphics pipeline layout is no longer bound");
                }
                const auto descriptor_status =
                    validate_required_descriptors(layout->second, pass.sampled_resources);
                if (!descriptor_status) {
                    return descriptor_status;
                }
                // Post-process passes carry their own constants; only scene draws consume the
                // chunk block the recorder pushes for them.
                if (pass.kind != rhi::RenderPassKind::post_process) {
                    const auto has_chunk_constants = std::ranges::any_of(
                        layout->second.desc.push_constant_ranges,
                        [](const rhi::RenderPushConstantRange& range) {
                            return rhi::any(range.stages & rhi::RenderShaderStageFlags::vertex) &&
                                   range.byte_offset == 0 &&
                                   range.byte_size >= sizeof(rhi::ChunkPushConstants);
                        });
                    if (!has_chunk_constants) {
                        return core::Status::failure(
                            "renderer.missing_chunk_push_constants",
                            "render draw pipeline layout must expose chunk push constants");
                    }
                }
            }

            const auto has_color_target = std::ranges::any_of(
                pass.writes, [&frame, reference_pipeline](const std::string& resource_name) {
                    const auto* resource = frame.plan.find_resource(resource_name);
                    return resource != nullptr && !rhi::is_depth_format(resource->format) &&
                           resource->format == reference_pipeline->desc.color_target_format;
                });
            const auto has_depth_target =
                std::ranges::any_of(pass.writes, [&frame](const std::string& resource_name) {
                    const auto* resource = frame.plan.find_resource(resource_name);
                    return resource != nullptr && rhi::is_depth_format(resource->format);
                });
            if (!has_color_target || (reference_pipeline->uses_depth && !has_depth_target)) {
                return core::Status::failure(
                    "renderer.incompatible_draw_targets",
                    "Vulkan render draw pass targets do not match its graphics pipeline");
            }
        }
        return core::Status::ok();
    }

    // Moves the resources a pass samples into a shader-readable layout and points this frame's
    // descriptor set at them. Graph resources carry no device handle, so the binding is resolved
    // here from the pool, every frame, rather than once at material bind time.
    [[nodiscard]] core::Status
    prepare_sampled_resources(const rhi::RenderFrameSubmission& frame,
                              const rhi::RenderPassDesc& pass, std::size_t pass_index,
                              VulkanFrameContext& frame_context, std::size_t frame_context_index,
                              VkCommandBuffer commands, std::size_t& submitted_barrier_count) {
        auto sampler_status = ensure_default_sampler();
        if (!sampler_status) {
            return sampler_status;
        }

        std::vector<VkDescriptorImageInfo> image_infos;
        image_infos.reserve(pass.sampled_resources.size());
        for (const auto& sampled : pass.sampled_resources) {
            auto* image = frame_context.resources.find(sampled.resource_name);
            if (image == nullptr || image->view == VK_NULL_HANDLE) {
                return core::Status::failure("renderer.vulkan_unbacked_sampled_resource",
                                             "frame graph pass '" + pass.name + "' samples '" +
                                                 sampled.resource_name +
                                                 "' which has no backing image");
            }
            if (image->layout != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
                // TOP_OF_PIPE supports no access flags, so an untouched image must declare none.
                const auto undefined = image->layout == VK_IMAGE_LAYOUT_UNDEFINED;
                VkImageMemoryBarrier to_read{};
                to_read.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
                to_read.srcAccessMask =
                    undefined ? 0 : static_cast<VkAccessFlags>(VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
                to_read.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
                to_read.oldLayout = image->layout;
                to_read.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                to_read.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                to_read.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                to_read.image = image->image;
                to_read.subresourceRange.aspectMask = static_cast<VkImageAspectFlags>(
                    image->is_depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT);
                to_read.subresourceRange.levelCount = 1;
                to_read.subresourceRange.layerCount = 1;
                vkCmdPipelineBarrier(commands,
                                     undefined ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT
                                               : VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                                     VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0,
                                     nullptr, 1, &to_read);
                image->layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                ++submitted_barrier_count;
            }
            image_infos.push_back(VkDescriptorImageInfo{default_sampler_, image->view,
                                                        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL});
        }

        // Which material's set to update follows from the draws recorded into this pass.
        const auto pass_commands = std::ranges::find_if(
            frame.pass_commands, [pass_index](const rhi::RenderPassCommands& candidate) {
                return candidate.pass_index == pass_index;
            });
        if (pass_commands == frame.pass_commands.end() || pass_commands->draws.empty()) {
            // Nothing draws here, so there is no set to point anywhere. The transitions above still
            // matter for whichever later pass samples the resource.
            return core::Status::ok();
        }
        const auto pipeline =
            graphics_pipelines_.find(pass_commands->draws.front().pipeline.value);
        if (pipeline == graphics_pipelines_.end()) {
            return core::Status::failure("renderer.unknown_graphics_pipeline",
                                         "sampling pass draw references an unknown pipeline");
        }
        const auto layout = pipeline_layouts_.find(pipeline->second.desc.material_id.value());
        if (layout == pipeline_layouts_.end()) {
            return core::Status::failure("renderer.unknown_pipeline_layout",
                                         "sampling pass draw references an unknown layout");
        }
        if (layout->second.per_frame_descriptor_sets.size() <= frame_context_index) {
            return core::Status::failure(
                "renderer.material_missing_per_frame_descriptors",
                "material '" + pipeline->second.desc.material_id.value() +
                    "' samples frame graph resources but its pipeline layout did not request "
                    "per-frame descriptors");
        }

        std::vector<VkWriteDescriptorSet> writes;
        writes.reserve(pass.sampled_resources.size());
        for (std::size_t index = 0; index < pass.sampled_resources.size(); ++index) {
            const auto* binding = find_descriptor_binding(
                layout->second.desc, pass.sampled_resources[index].binding_name);
            if (binding == nullptr) {
                return core::Status::failure(
                    "renderer.unknown_descriptor_binding",
                    "material '" + pipeline->second.desc.material_id.value() +
                        "' has no descriptor binding named '" +
                        pass.sampled_resources[index].binding_name + "'");
            }
            VkWriteDescriptorSet write{};
            write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            write.dstSet = layout->second.per_frame_descriptor_sets[frame_context_index];
            write.dstBinding = binding->slot;
            write.descriptorCount = 1;
            write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            write.pImageInfo = &image_infos[index];
            writes.push_back(write);
        }
        vkUpdateDescriptorSets(device_, static_cast<std::uint32_t>(writes.size()), writes.data(), 0,
                               nullptr);
        return core::Status::ok();
    }

    [[nodiscard]] core::Result<rhi::RenderFrameStats>
    execute_submitted_frame(const rhi::RenderFrameSubmission& frame) {
        using Clock = std::chrono::steady_clock;
        double gpu_wait_ms = 0.0;
        const auto accumulate_wait = [&gpu_wait_ms](Clock::time_point started) noexcept {
            gpu_wait_ms +=
                std::chrono::duration<double, std::milli>(Clock::now() - started).count();
        };

        auto resource_status = validate_submitted_frame_resources(frame);
        if (!resource_status) {
            return core::Result<rhi::RenderFrameStats>::failure(resource_status.error().code,
                                                                resource_status.error().message);
        }
        auto execution_plan = frame.plan.build_execution_plan();
        if (!execution_plan) {
            return core::Result<rhi::RenderFrameStats>::failure(execution_plan.error().code,
                                                                execution_plan.error().message);
        }

        const bool present = frame.plan.has_present_pass();
        if (present && surface_ == VK_NULL_HANDLE) {
            return core::Result<rhi::RenderFrameStats>::failure(
                "renderer.vulkan_present_unavailable",
                "Vulkan frame requests presentation without a native window surface");
        }
        if (desc_.initial_extent.width != frame.plan.extent.width ||
            desc_.initial_extent.height != frame.plan.extent.height) {
            const auto wait_started = Clock::now();
            const auto idle_result = vkDeviceWaitIdle(device_);
            accumulate_wait(wait_started);
            if (idle_result != VK_SUCCESS) {
                return core::Result<rhi::RenderFrameStats>::failure(
                    "renderer.vulkan_wait_idle_failed",
                    "failed to idle Vulkan device before frame resize: " +
                        std::string(vk_result_name(idle_result)));
            }
            complete_all_submissions();
            desc_.initial_extent = frame.plan.extent;
            destroy_swapchain();
            destroy_offscreen_target();
            for (auto& context : frame_contexts_) {
                destroy_frame_target(context.target);
            }
        }

        if (frame_contexts_.empty()) {
            return core::Result<rhi::RenderFrameStats>::failure(
                "renderer.vulkan_frame_context_unavailable",
                "Vulkan frame contexts were not initialized");
        }
        const auto frame_context_index = next_frame_context_;
        auto& frame_context = frame_contexts_[frame_context_index];
        auto context_status = wait_for_frame_context(frame_context, gpu_wait_ms);
        if (!context_status) {
            return core::Result<rhi::RenderFrameStats>::failure(context_status.error().code,
                                                                context_status.error().message);
        }
        const auto gpu_timing = collect_latest_gpu_timing();
        const auto frame_index = next_frame_index_;

        std::uint32_t image_index = 0;
        bool acquired_suboptimal = false;
        if (present) {
            auto status = ensure_swapchain();
            if (!status) {
                return core::Result<rhi::RenderFrameStats>::failure(status.error().code,
                                                                    status.error().message);
            }
            // A finite timeout keeps the native event loop responsive when a compositor stops
            // releasing images for an occluded or minimized FIFO swapchain.
            constexpr std::uint64_t acquire_timeout_nanoseconds = 50'000'000;
            auto wait_started = Clock::now();
            auto acquire_result =
                vkAcquireNextImageKHR(device_, swapchain_, acquire_timeout_nanoseconds,
                                      frame_context.image_available, VK_NULL_HANDLE, &image_index);
            accumulate_wait(wait_started);
            if (acquire_result == VK_ERROR_OUT_OF_DATE_KHR) {
                wait_started = Clock::now();
                const auto idle_result = vkDeviceWaitIdle(device_);
                accumulate_wait(wait_started);
                if (idle_result != VK_SUCCESS) {
                    return core::Result<rhi::RenderFrameStats>::failure(
                        "renderer.vulkan_wait_idle_failed",
                        "failed to idle Vulkan device for swapchain recreation: " +
                            std::string(vk_result_name(idle_result)));
                }
                complete_all_submissions();
                destroy_swapchain();
                status = ensure_swapchain();
                if (!status) {
                    return core::Result<rhi::RenderFrameStats>::failure(status.error().code,
                                                                        status.error().message);
                }
                wait_started = Clock::now();
                acquire_result = vkAcquireNextImageKHR(
                    device_, swapchain_, acquire_timeout_nanoseconds, frame_context.image_available,
                    VK_NULL_HANDLE, &image_index);
                accumulate_wait(wait_started);
            }
            if (acquire_result == VK_TIMEOUT || acquire_result == VK_NOT_READY) {
                rhi::RenderFrameStats stats;
                stats.backend = backend();
                stats.frame_index = frame_index;
                stats.submission_serial = last_submission_serial_;
                stats.completed_submission_serial = completed_submission_serial_;
                stats.extent = swapchain_extent_;
                stats.clear_color = frame.plan.first_clear_color();
                stats.presented = false;
                stats.render_pass_count = execution_plan.value().ordered_passes.size();
                stats.present_pass_count = execution_plan.value().present_pass_count;
                stats.resource_use_count = execution_plan.value().resource_uses.size();
                stats.dependency_count = execution_plan.value().dependencies.size();
                stats.transition_count = execution_plan.value().transitions.size();
                stats.cpu_gpu_wait_ms = gpu_wait_ms;
                attach_gpu_timing(stats, gpu_timing, frame_index);
                attach_latest_gpu_upload_timing(stats);
                return core::Result<rhi::RenderFrameStats>::success(stats);
            }
            acquired_suboptimal = acquire_result == VK_SUBOPTIMAL_KHR;
            if (acquire_result != VK_SUCCESS && !acquired_suboptimal) {
                return core::Result<rhi::RenderFrameStats>::failure(
                    "renderer.vulkan_acquire_image_failed",
                    "failed to acquire Vulkan swapchain image: " +
                        std::string(vk_result_name(acquire_result)));
            }
            if (image_index >= swapchain_images_.size()) {
                return core::Result<rhi::RenderFrameStats>::failure(
                    "renderer.vulkan_invalid_swapchain_image",
                    "Vulkan acquired an out-of-range swapchain image");
            }
        }

        const auto target_extent = present ? swapchain_extent_ : frame.plan.extent;
        auto status = ensure_frame_target(frame_context, target_extent);
        if (!status) {
            return core::Result<rhi::RenderFrameStats>::failure(status.error().code,
                                                                status.error().message);
        }

        // Back the resources the graph declares. Transients (the depth target today, the HDR scene
        // target once the recorder is fully graph-driven) come from the pool at the format the plan
        // asked for. The external output resource is backed by the offscreen colour image that the
        // frame blits to the swapchain.
        frame_context.resources.initialize(device_, physical_device_);
        std::vector<rhi::RenderResourceDesc> graph_resources(frame.plan.resources.begin(),
                                                             frame.plan.resources.end());
        for (auto& resource : graph_resources) {
            resource.extent = target_extent;
        }
        status = frame_context.resources.ensure_transients(graph_resources);
        if (!status) {
            return core::Result<rhi::RenderFrameStats>::failure(status.error().code,
                                                                status.error().message);
        }
        const auto* output_desc = frame.plan.find_resource(rhi::output_resource_name);
        frame_context.resources.bind_external(
            rhi::output_resource_name, frame_context.target.color_image,
            frame_context.target.color_view,
            output_desc == nullptr ? VK_FORMAT_R8G8B8A8_UNORM
                                   : vulkan_image_format(output_desc->format),
            frame_context.target.color_layout, target_extent);

        auto* graph_depth = frame_context.resources.find(rhi::depth_resource_name);
        if (graph_depth == nullptr) {
            return core::Result<rhi::RenderFrameStats>::failure(
                "renderer.vulkan_missing_depth_resource",
                "frame graph does not declare the depth resource the Vulkan backend requires");
        }

        const auto has_draws = !frame.pass_commands.empty();
        std::size_t frame_pipeline_bind_count = 0;
        VulkanGraphicsPipelineResource* first_pipeline = nullptr;
        if (has_draws) {
            const auto first_pipeline_it =
                graphics_pipelines_.find(frame.pass_commands.front().draws.front().pipeline.value);
            first_pipeline = &first_pipeline_it->second;
        }

        const auto frame_commands = frame_context.graphics_commands;
        const auto frame_fence = frame_context.completion_fence;
        const auto frame_image_available = frame_context.image_available;
        auto& frame_color_image = frame_context.target.color_image;
        // The colour attachment view now comes from the graph resource pool; the image and its
        // layout are still needed here for the barriers and the final blit.
        auto& frame_color_layout = frame_context.target.color_layout;
        auto& frame_depth_image = graph_depth->image;
        auto& frame_depth_view = graph_depth->view;
        auto& frame_depth_layout = graph_depth->layout;

        // Dynamic rendering binds image views directly, so there is no framebuffer object to
        // create, cache, or retire alongside the frame's submission serial.
        auto result = VK_SUCCESS;

        const auto command_recording_started = Clock::now();
        VkCommandBufferBeginInfo begin_info{};
        begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        result = vkBeginCommandBuffer(frame_commands, &begin_info);
        if (result != VK_SUCCESS) {
            return core::Result<rhi::RenderFrameStats>::failure(
                "renderer.vulkan_begin_command_buffer_failed",
                "failed to begin Vulkan terrain command buffer: " +
                    std::string(vk_result_name(result)));
        }
        begin_timestamp_frame(frame_commands, frame_index);

        const VkImageSubresourceRange color_range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        std::size_t submitted_barrier_count = 1;
        const auto clear_color = frame.plan.first_clear_color();
        if (has_draws) {
            VkImageMemoryBarrier color_to_attachment{};
            color_to_attachment.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            color_to_attachment.srcAccessMask =
                frame_color_layout == VK_IMAGE_LAYOUT_UNDEFINED
                    ? 0
                    : VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
            color_to_attachment.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
            color_to_attachment.oldLayout = frame_color_layout;
            color_to_attachment.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            color_to_attachment.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            color_to_attachment.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            color_to_attachment.image = frame_color_image;
            color_to_attachment.subresourceRange = color_range;
            vkCmdPipelineBarrier(frame_commands,
                                 frame_color_layout == VK_IMAGE_LAYOUT_UNDEFINED
                                     ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT
                                     : VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                 VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, nullptr, 0,
                                 nullptr, 1, &color_to_attachment);
            // The pool borrows this image, so its recorded layout has to follow the transition
            // just issued or the per-pass transition below would use a stale oldLayout.
            if (auto* output_image = frame_context.resources.find(rhi::output_resource_name);
                output_image != nullptr) {
                output_image->layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            }

            if (first_pipeline->uses_depth) {
                VkImageSubresourceRange depth_range{};
                depth_range.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
                if (vulkan_format_has_stencil(depth_format_)) {
                    depth_range.aspectMask |= VK_IMAGE_ASPECT_STENCIL_BIT;
                }
                depth_range.levelCount = 1;
                depth_range.layerCount = 1;
                VkImageMemoryBarrier depth_to_attachment{};
                depth_to_attachment.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
                depth_to_attachment.srcAccessMask =
                    frame_depth_layout == VK_IMAGE_LAYOUT_UNDEFINED
                        ? 0
                        : VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
                depth_to_attachment.dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                                                    VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
                depth_to_attachment.oldLayout = frame_depth_layout;
                depth_to_attachment.newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
                depth_to_attachment.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                depth_to_attachment.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                depth_to_attachment.image = frame_depth_image;
                depth_to_attachment.subresourceRange = depth_range;
                vkCmdPipelineBarrier(frame_commands,
                                     frame_depth_layout == VK_IMAGE_LAYOUT_UNDEFINED
                                         ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT
                                         : VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                                     VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT, 0, 0, nullptr, 0,
                                     nullptr, 1, &depth_to_attachment);
                ++submitted_barrier_count;
            }

            // Dynamic state is command-buffer scoped, so setting it once here applies to every
            // rendering block below.
            VkViewport viewport{};
            viewport.width = static_cast<float>(target_extent.width);
            viewport.height = static_cast<float>(target_extent.height);
            viewport.minDepth = 0.0F;
            viewport.maxDepth = 1.0F;
            vkCmdSetViewport(frame_commands, 0, 1, &viewport);
            VkRect2D scissor{};
            scissor.extent = VkExtent2D{target_extent.width, target_extent.height};
            vkCmdSetScissor(frame_commands, 0, 1, &scissor);

            VkPipeline bound_pipeline = VK_NULL_HANDLE;
            std::size_t pipeline_bind_count = 0;
            const auto find_commands =
                [&frame](std::string_view pass_name) -> const rhi::RenderPassCommands* {
                const auto found = std::ranges::find_if(
                    frame.pass_commands, [&frame, pass_name](const auto& commands) {
                        return frame.plan.passes[commands.pass_index].name == pass_name;
                    });
                return found == frame.pass_commands.end() ? nullptr : &*found;
            };
            const auto record_draws = [&](const rhi::RenderPassDesc& recorded_pass) {
                const auto* commands = find_commands(recorded_pass.name);
                if (commands == nullptr) {
                    return;
                }
                for (const auto& draw : commands->draws) {
                    auto& pipeline = graphics_pipelines_.at(draw.pipeline.value);
                    auto& layout = pipeline_layouts_.at(pipeline.desc.material_id.value());
                    const auto indexed = draw.index_buffer.is_valid();
                    if (bound_pipeline != pipeline.pipeline) {
                        bound_pipeline = pipeline.pipeline;
                        ++pipeline_bind_count;
                        vkCmdBindPipeline(frame_commands, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                          pipeline.pipeline);
                        // A material sampling graph resources binds this frame's own set, which
                        // was just pointed at the current frame's images.
                        const auto descriptor_set =
                            layout.per_frame_descriptor_sets.empty()
                                ? layout.descriptor_set
                                : layout.per_frame_descriptor_sets[frame_context_index];
                        if (descriptor_set != VK_NULL_HANDLE) {
                            vkCmdBindDescriptorSets(frame_commands, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                                    layout.pipeline_layout, 0, 1, &descriptor_set,
                                                    0, nullptr);
                        }
                    }
                    if (draw.vertex_buffer.is_valid()) {
                        auto& vertex = buffer_resources_.at(draw.vertex_buffer.value);
                        const VkDeviceSize vertex_buffer_offset = 0;
                        vkCmdBindVertexBuffers(frame_commands, 0, 1, &vertex.buffer,
                                               &vertex_buffer_offset);
                    }
                    if (indexed) {
                        auto& index = buffer_resources_.at(draw.index_buffer.value);
                        vkCmdBindIndexBuffer(frame_commands, index.buffer, 0,
                                             draw.index_type == rhi::RenderIndexType::uint16
                                                 ? VK_INDEX_TYPE_UINT16
                                                 : VK_INDEX_TYPE_UINT32);
                    }
                    VkRect2D draw_scissor{};
                    if (draw.scissor_enabled) {
                        draw_scissor.offset = {static_cast<std::int32_t>(draw.scissor.x),
                                               static_cast<std::int32_t>(draw.scissor.y)};
                        draw_scissor.extent = {draw.scissor.width, draw.scissor.height};
                    } else {
                        draw_scissor.extent = {target_extent.width, target_extent.height};
                    }
                    vkCmdSetScissor(frame_commands, 0, 1, &draw_scissor);
                    // A post-process pass resolves an image rather than shading the world, so it
                    // takes exposure and tone curve constants instead of the chunk block.
                    if (recorded_pass.kind == rhi::RenderPassKind::post_process) {
                        const auto constants = rhi::make_tone_map_push_constants(frame.exposure);
                        vkCmdPushConstants(frame_commands, layout.pipeline_layout,
                                           VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(constants),
                                           &constants);
                    } else {
                        const rhi::ChunkPushConstants constants{
                            frame.camera.view_projection,
                            {draw.camera_relative_origin.x, draw.camera_relative_origin.y,
                             draw.camera_relative_origin.z,
                             std::bit_cast<float>(draw.texture_variation_seed)},
                            {frame.environment.sun_direction.x, frame.environment.sun_direction.y,
                             frame.environment.sun_direction.z, frame.environment.sun_intensity},
                            {frame.environment.ambient_color.x, frame.environment.ambient_color.y,
                             frame.environment.ambient_color.z, frame.environment.fog_start},
                            {frame.environment.fog_color.x, frame.environment.fog_color.y,
                             frame.environment.fog_color.z, frame.environment.fog_end},
                        };
                        vkCmdPushConstants(frame_commands, layout.pipeline_layout,
                                           VK_SHADER_STAGE_VERTEX_BIT |
                                               VK_SHADER_STAGE_FRAGMENT_BIT,
                                           0, sizeof(constants), &constants);
                    }
                    if (indexed) {
                        vkCmdDrawIndexed(frame_commands, draw.index_count, draw.instance_count,
                                         draw.first_index, draw.vertex_offset, draw.first_instance);
                    } else {
                        vkCmdDraw(frame_commands, draw.vertex_count, draw.instance_count, 0,
                                  draw.first_instance);
                    }
                }
            };

            // Each pass records into its own rendering block, bound to the attachments the pass
            // declares it writes. A single frame-wide block could only ever target one image set,
            // which is why a post-process pass reading what the world wrote was not expressible.
            const auto colour_write_of =
                [](const rhi::RenderPassDesc& pass) -> const std::string* {
                for (const auto& written : pass.writes) {
                    if (written != rhi::depth_resource_name) {
                        return &written;
                    }
                }
                return nullptr;
            };
            const auto writes_depth = [](const rhi::RenderPassDesc& pass) {
                return std::ranges::any_of(pass.writes, [](const std::string& written) {
                    return written == rhi::depth_resource_name;
                });
            };

            // Whether a pass binds depth follows from the pipelines recorded into that pass, not
            // from the frame's first pipeline: a post-process pass draws with a depth-free pipeline
            // while the world passes around it do not, and binding an attachment the pipeline was
            // not created against is invalid under dynamic rendering. A pass with no draws has
            // nothing to stay compatible with, so it follows the frame default and still clears
            // depth for whatever comes after it.
            const auto pass_binds_depth = [&](std::size_t index) {
                const auto& candidate = frame.plan.passes[index];
                if (candidate.kind == rhi::RenderPassKind::present || !writes_depth(candidate)) {
                    return false;
                }
                const auto pass_commands = std::ranges::find_if(
                    frame.pass_commands, [index](const rhi::RenderPassCommands& commands) {
                        return commands.pass_index == index;
                    });
                if (pass_commands == frame.pass_commands.end() || pass_commands->draws.empty()) {
                    return first_pipeline->uses_depth;
                }
                const auto pipeline =
                    graphics_pipelines_.find(pass_commands->draws.front().pipeline.value);
                return pipeline == graphics_pipelines_.end() ? first_pipeline->uses_depth
                                                             : pipeline->second.uses_depth;
            };

            // Depth has to survive between blocks now that every pass opens its own, so only the
            // final pass that actually binds depth may discard it.
            std::vector<bool> binds_depth(frame.plan.passes.size(), false);
            std::size_t last_depth_pass = 0;
            for (std::size_t index = 0; index < frame.plan.passes.size(); ++index) {
                binds_depth[index] = pass_binds_depth(index);
                if (binds_depth[index]) {
                    last_depth_pass = index;
                }
            }

            struct PassPresentation {
                const char* label;
                float red;
                float green;
                float blue;
                bool timed;
                TimestampIndex start;
                TimestampIndex end;
            };
            const auto presentation_for = [&](std::string_view name) -> PassPresentation {
                if (name == "sky") {
                    return {"Sky gradient pass", 0.20F, 0.42F, 0.86F, false, {}, {}};
                }
                if (name == "opaque_terrain") {
                    return {"Opaque terrain pass", 0.20F,           0.72F,
                            0.28F,                 true,            opaque_start_timestamp,
                            opaque_end_timestamp};
                }
                if (name == "alpha_tested_terrain") {
                    return {"Alpha-tested terrain pass",
                            0.46F,
                            0.76F,
                            0.24F,
                            true,
                            alpha_tested_start_timestamp,
                            alpha_tested_end_timestamp};
                }
                if (name == "rich_static_instances") {
                    return {"Rich/static instances pass", 0.75F, 0.62F, 0.20F, false, {}, {}};
                }
                if (name == "transparent_terrain") {
                    return {"Transparent terrain and fluids pass",
                            0.20F,
                            0.60F,
                            0.86F,
                            true,
                            transparent_start_timestamp,
                            transparent_end_timestamp};
                }
                if (name == "debug") {
                    return {"Debug pass", 0.86F, 0.28F, 0.76F, false, {}, {}};
                }
                if (name == "ui") {
                    return {"UI pass", 0.80F, 0.80F, 0.80F, false, {}, {}};
                }
                if (name == "tone_map") {
                    return {"Tone mapping pass", 0.94F, 0.62F, 0.24F, false, {}, {}};
                }
                return {"Graph pass", 0.60F, 0.60F, 0.60F, false, {}, {}};
            };

            std::unordered_set<std::string> cleared_colour;
            bool depth_cleared = false;
            for (std::size_t pass_index = 0; pass_index < frame.plan.passes.size(); ++pass_index) {
                const auto& pass = frame.plan.passes[pass_index];
                if (pass.kind == rhi::RenderPassKind::present) {
                    continue;
                }
                const auto* colour_name = colour_write_of(pass);
                if (colour_name == nullptr) {
                    continue;
                }
                auto* colour = frame_context.resources.find(*colour_name);
                if (colour == nullptr || colour->view == VK_NULL_HANDLE) {
                    return core::Result<rhi::RenderFrameStats>::failure(
                        "renderer.vulkan_unbacked_frame_resource",
                        "frame graph pass '" + pass.name + "' writes resource '" + *colour_name +
                            "' which has no backing image");
                }

                // Resources this pass samples were written as colour attachments by an earlier
                // pass, so they need moving to a shader-readable layout and this frame's
                // descriptor set needs pointing at them before anything is recorded.
                if (!pass.sampled_resources.empty()) {
                    auto sampled_status = prepare_sampled_resources(
                        frame, pass, pass_index, frame_context, frame_context_index, frame_commands,
                        submitted_barrier_count);
                    if (!sampled_status) {
                        return core::Result<rhi::RenderFrameStats>::failure(
                            sampled_status.error().code, sampled_status.error().message);
                    }
                }

                // A transient graph target starts each frame undefined, and a target an earlier
                // pass sampled is left in shader-read layout, so move it back before writing.
                if (colour->layout != VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL) {
                    const auto undefined = colour->layout == VK_IMAGE_LAYOUT_UNDEFINED;
                    VkImageMemoryBarrier to_attachment{};
                    to_attachment.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
                    to_attachment.srcAccessMask =
                        undefined ? 0 : static_cast<VkAccessFlags>(VK_ACCESS_SHADER_READ_BIT);
                    to_attachment.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
                    to_attachment.oldLayout = colour->layout;
                    to_attachment.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                    to_attachment.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                    to_attachment.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                    to_attachment.image = colour->image;
                    to_attachment.subresourceRange = color_range;
                    vkCmdPipelineBarrier(frame_commands,
                                         undefined ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT
                                                   : VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                                         VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0,
                                         nullptr, 0, nullptr, 1, &to_attachment);
                    colour->layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                    ++submitted_barrier_count;
                }

                VkRenderingAttachmentInfo colour_attachment{};
                colour_attachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
                colour_attachment.imageView = colour->view;
                colour_attachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                colour_attachment.loadOp = cleared_colour.contains(*colour_name)
                                               ? VK_ATTACHMENT_LOAD_OP_LOAD
                                               : VK_ATTACHMENT_LOAD_OP_CLEAR;
                colour_attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
                colour_attachment.clearValue.color = VkClearColorValue{
                    {clear_color.red, clear_color.green, clear_color.blue, clear_color.alpha}};

                // Pipelines declare their depth format at creation, so a pass may only bind depth
                // when the pipelines recorded into it were built expecting one.
                const auto bind_depth = binds_depth[pass_index];
                VkRenderingAttachmentInfo depth_attachment{};
                depth_attachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
                depth_attachment.imageView = frame_depth_view;
                depth_attachment.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
                depth_attachment.loadOp = depth_cleared ? VK_ATTACHMENT_LOAD_OP_LOAD
                                                        : VK_ATTACHMENT_LOAD_OP_CLEAR;
                depth_attachment.storeOp = pass_index == last_depth_pass
                                               ? VK_ATTACHMENT_STORE_OP_DONT_CARE
                                               : VK_ATTACHMENT_STORE_OP_STORE;
                depth_attachment.clearValue.depthStencil = VkClearDepthStencilValue{1.0F, 0};

                VkRenderingInfo rendering_info{};
                rendering_info.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
                rendering_info.renderArea.extent =
                    VkExtent2D{target_extent.width, target_extent.height};
                rendering_info.layerCount = 1;
                rendering_info.colorAttachmentCount = 1;
                rendering_info.pColorAttachments = &colour_attachment;
                rendering_info.pDepthAttachment = bind_depth ? &depth_attachment : nullptr;

                const auto presentation = presentation_for(pass.name);
                begin_debug_label(frame_commands, presentation.label, presentation.red,
                                  presentation.green, presentation.blue);
                if (presentation.timed) {
                    write_timestamp(frame_commands, frame_index, presentation.start,
                                    VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT);
                }
                vkCmdBeginRendering(frame_commands, &rendering_info);
                record_draws(pass);
                vkCmdEndRendering(frame_commands);
                if (presentation.timed) {
                    write_timestamp(frame_commands, frame_index, presentation.end,
                                    VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
                }
                end_debug_label(frame_commands);

                cleared_colour.insert(*colour_name);
                if (bind_depth) {
                    depth_cleared = true;
                }
            }
            frame_pipeline_bind_count = pipeline_bind_count;
        } else {
            VkImageMemoryBarrier color_to_clear{};
            color_to_clear.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            color_to_clear.srcAccessMask =
                frame_color_layout == VK_IMAGE_LAYOUT_UNDEFINED
                    ? 0
                    : VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
            color_to_clear.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            color_to_clear.oldLayout = frame_color_layout;
            color_to_clear.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            color_to_clear.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            color_to_clear.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            color_to_clear.image = frame_color_image;
            color_to_clear.subresourceRange = color_range;
            vkCmdPipelineBarrier(frame_commands,
                                 frame_color_layout == VK_IMAGE_LAYOUT_UNDEFINED
                                     ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT
                                     : VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                 VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                                 &color_to_clear);
            const VkClearColorValue clear_value{
                {clear_color.red, clear_color.green, clear_color.blue, clear_color.alpha}};
            vkCmdClearColorImage(frame_commands, frame_color_image,
                                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear_value, 1,
                                 &color_range);
            write_timestamp(frame_commands, frame_index, opaque_start_timestamp,
                            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT);
            write_timestamp(frame_commands, frame_index, opaque_end_timestamp,
                            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT);
            write_timestamp(frame_commands, frame_index, alpha_tested_start_timestamp,
                            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT);
            write_timestamp(frame_commands, frame_index, alpha_tested_end_timestamp,
                            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT);
            write_timestamp(frame_commands, frame_index, transparent_start_timestamp,
                            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT);
            write_timestamp(frame_commands, frame_index, transparent_end_timestamp,
                            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT);
        }

        begin_debug_label(frame_commands, "Frame transfer", 0.30F, 0.48F, 0.92F);
        write_timestamp(frame_commands, frame_index, transfer_start_timestamp,
                        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT);

        VkImageMemoryBarrier color_to_transfer_source{};
        color_to_transfer_source.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        color_to_transfer_source.srcAccessMask =
            has_draws ? VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT : VK_ACCESS_TRANSFER_WRITE_BIT;
        color_to_transfer_source.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        color_to_transfer_source.oldLayout = has_draws ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL
                                                       : VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        color_to_transfer_source.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        color_to_transfer_source.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        color_to_transfer_source.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        color_to_transfer_source.image = frame_color_image;
        color_to_transfer_source.subresourceRange = color_range;
        vkCmdPipelineBarrier(frame_commands,
                             has_draws ? VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT
                                       : VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                             &color_to_transfer_source);
        ++submitted_barrier_count;

        begin_debug_label(frame_commands, "Final copy", 0.72F, 0.34F, 0.90F);
        write_timestamp(frame_commands, frame_index, final_copy_start_timestamp,
                        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT);
        if (present) {
            VkImageMemoryBarrier swapchain_to_transfer{};
            swapchain_to_transfer.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            swapchain_to_transfer.srcAccessMask = 0;
            swapchain_to_transfer.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            swapchain_to_transfer.oldLayout = swapchain_image_layouts_[image_index];
            swapchain_to_transfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            swapchain_to_transfer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            swapchain_to_transfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            swapchain_to_transfer.image = swapchain_images_[image_index];
            swapchain_to_transfer.subresourceRange = color_range;
            vkCmdPipelineBarrier(frame_commands,
                                 swapchain_image_layouts_[image_index] == VK_IMAGE_LAYOUT_UNDEFINED
                                     ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT
                                     : VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                                 VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                                 &swapchain_to_transfer);

            VkImageBlit blit{};
            blit.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            blit.srcSubresource.layerCount = 1;
            blit.srcOffsets[1] = VkOffset3D{static_cast<std::int32_t>(target_extent.width),
                                            static_cast<std::int32_t>(target_extent.height), 1};
            blit.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            blit.dstSubresource.layerCount = 1;
            blit.dstOffsets[1] = VkOffset3D{static_cast<std::int32_t>(swapchain_extent_.width),
                                            static_cast<std::int32_t>(swapchain_extent_.height), 1};
            vkCmdBlitImage(frame_commands, frame_color_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           swapchain_images_[image_index], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                           &blit, VK_FILTER_NEAREST);

            VkImageMemoryBarrier swapchain_to_present{};
            swapchain_to_present.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            swapchain_to_present.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            swapchain_to_present.dstAccessMask = 0;
            swapchain_to_present.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            swapchain_to_present.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
            swapchain_to_present.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            swapchain_to_present.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            swapchain_to_present.image = swapchain_images_[image_index];
            swapchain_to_present.subresourceRange = color_range;
            vkCmdPipelineBarrier(frame_commands, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                 VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 1,
                                 &swapchain_to_present);
            submitted_barrier_count += 2;
        }
        write_timestamp(frame_commands, frame_index, final_copy_end_timestamp,
                        VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
        end_debug_label(frame_commands);
        write_timestamp(frame_commands, frame_index, transfer_end_timestamp,
                        VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
        end_debug_label(frame_commands);
        write_timestamp(frame_commands, frame_index, frame_end_timestamp,
                        VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);

        result = vkEndCommandBuffer(frame_commands);
        if (result != VK_SUCCESS) {
            return core::Result<rhi::RenderFrameStats>::failure(
                "renderer.vulkan_end_command_buffer_failed",
                "failed to end Vulkan terrain command buffer: " +
                    std::string(vk_result_name(result)));
        }
        const auto command_recording_ms =
            std::chrono::duration<double, std::milli>(Clock::now() - command_recording_started)
                .count();

        const VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        VkSubmitInfo submit_info{};
        submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        if (present) {
            submit_info.waitSemaphoreCount = 1;
            submit_info.pWaitSemaphores = &frame_image_available;
            submit_info.pWaitDstStageMask = &wait_stage;
            submit_info.signalSemaphoreCount = 1;
            submit_info.pSignalSemaphores = &swapchain_render_finished_semaphores_[image_index];
        }
        submit_info.commandBufferCount = 1;
        submit_info.pCommandBuffers = &frame_commands;
        const auto submission_serial = last_submission_serial_ + 1;
        result = vkQueueSubmit(queue_, 1, &submit_info, frame_fence);
        if (result != VK_SUCCESS) {
            return core::Result<rhi::RenderFrameStats>::failure(
                "renderer.vulkan_queue_submit_failed",
                "failed to submit unified Vulkan terrain frame: " +
                    std::string(vk_result_name(result)));
        }
        last_submission_serial_ = submission_serial;
        frame_context.submission_serial = submission_serial;
        frame_context.in_flight = true;
        pending_frame_submissions_.push_back(submission_serial);
        ++next_frame_index_;
        next_frame_context_ = (next_frame_context_ + 1) % frame_contexts_.size();
        mark_timestamp_frame_pending(frame_index);
        for (const auto& commands : frame.pass_commands) {
            for (const auto& draw : commands.draws) {
                const auto pipeline = graphics_pipelines_.find(draw.pipeline.value);
                if (pipeline == graphics_pipelines_.end()) {
                    continue;
                }
                const auto layout =
                    pipeline_layouts_.find(pipeline->second.desc.material_id.value());
                if (layout != pipeline_layouts_.end()) {
                    layout->second.last_used_submission_serial = submission_serial;
                }
            }
        }

        // Render-finished semaphores are owned by swapchain images. Reacquiring an image proves
        // that presentation consumed its prior wait before that semaphore is signaled again.
        VkResult present_result = VK_SUCCESS;
        if (present) {
            VkPresentInfoKHR present_info{};
            present_info.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
            present_info.waitSemaphoreCount = 1;
            present_info.pWaitSemaphores = &swapchain_render_finished_semaphores_[image_index];
            present_info.swapchainCount = 1;
            present_info.pSwapchains = &swapchain_;
            present_info.pImageIndices = &image_index;
            present_result = vkQueuePresentKHR(queue_, &present_info);
        }

        frame_color_layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        if (has_draws && first_pipeline->uses_depth) {
            frame_depth_layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        }
        bool presented = present;
        if (present && present_result != VK_SUCCESS && present_result != VK_SUBOPTIMAL_KHR &&
            present_result != VK_ERROR_OUT_OF_DATE_KHR) {
            return core::Result<rhi::RenderFrameStats>::failure(
                "renderer.vulkan_present_failed",
                "failed to present unified Vulkan terrain frame: " +
                    std::string(vk_result_name(present_result)));
        }
        if (present) {
            swapchain_image_layouts_[image_index] = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        }
        if (present && (present_result == VK_ERROR_OUT_OF_DATE_KHR ||
                        present_result == VK_SUBOPTIMAL_KHR || acquired_suboptimal)) {
            presented = present_result != VK_ERROR_OUT_OF_DATE_KHR;
            const auto wait_started = Clock::now();
            const auto queue_idle_result = vkQueueWaitIdle(queue_);
            accumulate_wait(wait_started);
            if (queue_idle_result != VK_SUCCESS) {
                return core::Result<rhi::RenderFrameStats>::failure(
                    "renderer.vulkan_wait_idle_failed",
                    "failed to idle Vulkan present queue for swapchain recreation: " +
                        std::string(vk_result_name(queue_idle_result)));
            }
            complete_all_submissions();
            destroy_swapchain();
            const auto recreate_status = ensure_swapchain();
            if (!recreate_status) {
                return core::Result<rhi::RenderFrameStats>::failure(
                    recreate_status.error().code, recreate_status.error().message);
            }
        }

        rhi::RenderFrameStats stats;
        stats.backend = backend();
        stats.frame_index = frame_index;
        stats.submission_serial = submission_serial;
        stats.completed_submission_serial = completed_submission_serial_;
        stats.extent = target_extent;
        stats.clear_color = clear_color;
        stats.presented = presented;
        stats.render_pass_count = execution_plan.value().ordered_passes.size();
        stats.present_pass_count = execution_plan.value().present_pass_count;
        stats.resource_use_count = execution_plan.value().resource_uses.size();
        stats.dependency_count = execution_plan.value().dependencies.size();
        stats.transition_count = execution_plan.value().transitions.size();
        stats.synchronization_barrier_count = execution_plan.value().transitions.size();
        stats.submitted_synchronization_barrier_count = submitted_barrier_count;
        stats.pipeline_bind_count = frame_pipeline_bind_count;
        stats.cpu_command_recording_ms = command_recording_ms;
        stats.cpu_gpu_wait_ms = gpu_wait_ms;
        attach_gpu_timing(stats, gpu_timing, frame_index);
        attach_latest_gpu_upload_timing(stats);
        for (const auto& commands : frame.pass_commands) {
            const auto& pass = frame.plan.passes[commands.pass_index];
            stats.draw_count += commands.draws.size();
            stats.indexed_draw_count += commands.draws.size();
            if (pass.name == "opaque_terrain") {
                stats.opaque_terrain_draw_count += commands.draws.size();
            } else if (pass.name == "alpha_tested_terrain") {
                stats.alpha_tested_terrain_draw_count += commands.draws.size();
            } else if (pass.name == "transparent_terrain") {
                stats.transparent_terrain_draw_count += commands.draws.size();
            }
            for (const auto& draw : commands.draws) {
                stats.total_indices +=
                    static_cast<std::size_t>(draw.index_count) * draw.instance_count;
            }
        }
        return core::Result<rhi::RenderFrameStats>::success(stats);
    }

    [[nodiscard]] core::Status
    submit_offscreen_mesh_draws(std::span<const rhi::RenderMeshBinding> draws) {
        if (draws.empty()) {
            return core::Status::failure("renderer.empty_draw_list",
                                         "mesh draw list must contain at least one draw");
        }

        auto status = ensure_offscreen_target(current_extent());
        if (!status) {
            return status;
        }

        auto* first_pipeline = find_graphics_pipeline_for_material(draws.front().material_id);
        if (first_pipeline == nullptr) {
            return core::Status::failure("renderer.unbound_material_graphics_pipeline",
                                         "mesh draw material must have a graphics pipeline");
        }

        auto result = vkResetFences(device_, 1, &fence_);
        if (result != VK_SUCCESS) {
            return core::Status::failure("renderer.vulkan_reset_fence_failed",
                                         "failed to reset Vulkan fence: " +
                                             std::string(vk_result_name(result)));
        }

        result = vkResetCommandBuffer(command_buffer_, 0);
        if (result != VK_SUCCESS) {
            return core::Status::failure("renderer.vulkan_reset_command_buffer_failed",
                                         "failed to reset Vulkan command buffer: " +
                                             std::string(vk_result_name(result)));
        }

        VkCommandBufferBeginInfo begin_info{};
        begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        result = vkBeginCommandBuffer(command_buffer_, &begin_info);
        if (result != VK_SUCCESS) {
            return core::Status::failure("renderer.vulkan_begin_command_buffer_failed",
                                         "failed to begin Vulkan draw command buffer: " +
                                             std::string(vk_result_name(result)));
        }

        VkPipelineStageFlags source_stage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        VkAccessFlags source_access = 0;
        if (offscreen_layout_ != VK_IMAGE_LAYOUT_UNDEFINED) {
            source_stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
            source_access = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
        }

        const VkImageSubresourceRange color_range{
            VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1,
        };
        VkImageMemoryBarrier to_color_attachment{};
        to_color_attachment.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        to_color_attachment.srcAccessMask = source_access;
        to_color_attachment.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        to_color_attachment.oldLayout = offscreen_layout_;
        to_color_attachment.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        to_color_attachment.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        to_color_attachment.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        to_color_attachment.image = offscreen_image_;
        to_color_attachment.subresourceRange = color_range;
        vkCmdPipelineBarrier(command_buffer_, source_stage,
                             VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, nullptr, 0,
                             nullptr, 1, &to_color_attachment);

        VkRenderingAttachmentInfo color_attachment{};
        color_attachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        color_attachment.imageView = offscreen_image_view_;
        color_attachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        color_attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        color_attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        color_attachment.clearValue.color = VkClearColorValue{{0.0F, 0.0F, 0.0F, 1.0F}};

        VkRenderingInfo rendering_info{};
        rendering_info.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
        rendering_info.renderArea.offset = VkOffset2D{0, 0};
        rendering_info.renderArea.extent =
            VkExtent2D{offscreen_extent_.width, offscreen_extent_.height};
        rendering_info.layerCount = 1;
        rendering_info.colorAttachmentCount = 1;
        rendering_info.pColorAttachments = &color_attachment;
        vkCmdBeginRendering(command_buffer_, &rendering_info);

        VkViewport viewport{};
        viewport.x = 0.0F;
        viewport.y = 0.0F;
        viewport.width = static_cast<float>(offscreen_extent_.width);
        viewport.height = static_cast<float>(offscreen_extent_.height);
        viewport.minDepth = 0.0F;
        viewport.maxDepth = 1.0F;
        vkCmdSetViewport(command_buffer_, 0, 1, &viewport);

        VkRect2D scissor{};
        scissor.offset = VkOffset2D{0, 0};
        scissor.extent = VkExtent2D{offscreen_extent_.width, offscreen_extent_.height};
        vkCmdSetScissor(command_buffer_, 0, 1, &scissor);

        for (const auto& draw : draws) {
            auto* pipeline = find_graphics_pipeline_for_material(draw.material_id);
            if (pipeline == nullptr) {
                vkCmdEndRendering(command_buffer_);
                vkEndCommandBuffer(command_buffer_);
                return core::Status::failure("renderer.unbound_material_graphics_pipeline",
                                             "mesh draw material must have a graphics pipeline");
            }
            const auto vertex = buffer_resources_.find(draw.vertex_buffer.value);
            if (vertex == buffer_resources_.end()) {
                vkCmdEndRendering(command_buffer_);
                vkEndCommandBuffer(command_buffer_);
                return core::Status::failure(
                    "renderer.unknown_vertex_buffer",
                    "mesh draw references a vertex buffer handle not owned by this device");
            }

            vkCmdBindPipeline(command_buffer_, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline->pipeline);
            const VkDeviceSize vertex_offset = 0;
            vkCmdBindVertexBuffers(command_buffer_, 0, 1, &vertex->second.buffer, &vertex_offset);

            if (draw.index_buffer.is_valid()) {
                const auto index = buffer_resources_.find(draw.index_buffer.value);
                if (index == buffer_resources_.end()) {
                    vkCmdEndRendering(command_buffer_);
                    vkEndCommandBuffer(command_buffer_);
                    return core::Status::failure(
                        "renderer.unknown_index_buffer",
                        "mesh draw references an index buffer handle not owned by this device");
                }
                vkCmdBindIndexBuffer(command_buffer_, index->second.buffer, 0,
                                     VK_INDEX_TYPE_UINT32);
                vkCmdDrawIndexed(command_buffer_, draw.index_count, draw.instance_count, 0, 0, 0);
            } else {
                vkCmdDraw(command_buffer_, draw.vertex_count, draw.instance_count, 0, 0);
            }
        }

        vkCmdEndRendering(command_buffer_);

        result = vkEndCommandBuffer(command_buffer_);
        if (result != VK_SUCCESS) {
            return core::Status::failure("renderer.vulkan_end_command_buffer_failed",
                                         "failed to end Vulkan draw command buffer: " +
                                             std::string(vk_result_name(result)));
        }

        VkSubmitInfo submit_info{};
        submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit_info.commandBufferCount = 1;
        submit_info.pCommandBuffers = &command_buffer_;
        result = vkQueueSubmit(queue_, 1, &submit_info, fence_);
        if (result != VK_SUCCESS) {
            return core::Status::failure("renderer.vulkan_queue_submit_failed",
                                         "failed to submit Vulkan draw command buffer: " +
                                             std::string(vk_result_name(result)));
        }

        result = vkWaitForFences(device_, 1, &fence_, VK_TRUE,
                                 std::numeric_limits<std::uint64_t>::max());
        if (result != VK_SUCCESS) {
            return core::Status::failure("renderer.vulkan_wait_fence_failed",
                                         "failed to wait for Vulkan draw fence: " +
                                             std::string(vk_result_name(result)));
        }

        offscreen_layout_ = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        return core::Status::ok();
    }

    [[nodiscard]] core::Status
    submit_swapchain_clear(std::uint32_t image_index, rhi::ClearColor clear_color,
                           std::span<const VulkanFrameTransition> frame_transitions) {
        auto result = vkResetFences(device_, 1, &fence_);
        if (result != VK_SUCCESS) {
            return core::Status::failure("renderer.vulkan_reset_fence_failed",
                                         "failed to reset Vulkan fence: " +
                                             std::string(vk_result_name(result)));
        }

        result = vkResetCommandBuffer(command_buffer_, 0);
        if (result != VK_SUCCESS) {
            return core::Status::failure("renderer.vulkan_reset_command_buffer_failed",
                                         "failed to reset Vulkan command buffer: " +
                                             std::string(vk_result_name(result)));
        }

        VkCommandBufferBeginInfo begin_info{};
        begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        result = vkBeginCommandBuffer(command_buffer_, &begin_info);
        if (result != VK_SUCCESS) {
            return core::Status::failure("renderer.vulkan_begin_command_buffer_failed",
                                         "failed to begin Vulkan command buffer: " +
                                             std::string(vk_result_name(result)));
        }

        auto old_layout = swapchain_image_layouts_[image_index];

        const VkImageSubresourceRange clear_range{
            VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1,
        };

        record_planned_image_transition_barriers(command_buffer_, swapchain_images_[image_index],
                                                 clear_range, old_layout, frame_transitions);
        VkPipelineStageFlags source_stage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        VkAccessFlags source_access = 0;
        if (old_layout != VK_IMAGE_LAYOUT_UNDEFINED) {
            source_stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
            source_access = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
        }

        VkImageMemoryBarrier to_transfer{};
        to_transfer.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        to_transfer.srcAccessMask = source_access;
        to_transfer.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        to_transfer.oldLayout = old_layout;
        to_transfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        to_transfer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        to_transfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        to_transfer.image = swapchain_images_[image_index];
        to_transfer.subresourceRange = clear_range;
        vkCmdPipelineBarrier(command_buffer_, source_stage, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0,
                             nullptr, 0, nullptr, 1, &to_transfer);

        const VkClearColorValue vk_clear_color{
            {clear_color.red, clear_color.green, clear_color.blue, clear_color.alpha}};
        vkCmdClearColorImage(command_buffer_, swapchain_images_[image_index],
                             VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &vk_clear_color, 1,
                             &clear_range);

        VkImageMemoryBarrier to_present{};
        to_present.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        to_present.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        to_present.dstAccessMask = 0;
        to_present.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        to_present.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        to_present.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        to_present.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        to_present.image = swapchain_images_[image_index];
        to_present.subresourceRange = clear_range;
        vkCmdPipelineBarrier(command_buffer_, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 1,
                             &to_present);

        result = vkEndCommandBuffer(command_buffer_);
        if (result != VK_SUCCESS) {
            return core::Status::failure("renderer.vulkan_end_command_buffer_failed",
                                         "failed to end Vulkan command buffer: " +
                                             std::string(vk_result_name(result)));
        }

        const VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        VkSubmitInfo submit_info{};
        submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit_info.waitSemaphoreCount = 1;
        submit_info.pWaitSemaphores = &image_available_semaphore_;
        submit_info.pWaitDstStageMask = &wait_stage;
        submit_info.commandBufferCount = 1;
        submit_info.pCommandBuffers = &command_buffer_;
        submit_info.signalSemaphoreCount = 1;
        submit_info.pSignalSemaphores = &render_finished_semaphore_;
        result = vkQueueSubmit(queue_, 1, &submit_info, fence_);
        if (result != VK_SUCCESS) {
            return core::Status::failure("renderer.vulkan_queue_submit_failed",
                                         "failed to submit Vulkan present command buffer: " +
                                             std::string(vk_result_name(result)));
        }

        return core::Status::ok();
    }

    [[nodiscard]] core::Result<std::size_t>
    submit_offscreen_clear(rhi::ClearColor clear_color,
                           std::span<const VulkanFrameTransition> frame_transitions,
                           std::span<const VulkanFrameTransition> target_transitions) {
        std::vector<VulkanFrameImageResource> frame_images;
        const auto target_resource = target_transitions.empty()
                                         ? std::string_view{}
                                         : target_transitions.front().resource_name;
        for (const auto& resource_name : transition_resource_names(frame_transitions)) {
            if (resource_name == target_resource) {
                continue;
            }
            auto image = create_frame_image_resource(resource_name, offscreen_extent_);
            if (!image) {
                destroy_frame_image_resources(frame_images);
                return core::Result<std::size_t>::failure(image.error().code,
                                                          image.error().message);
            }
            frame_images.push_back(std::move(image.value()));
        }

        auto result = vkResetFences(device_, 1, &fence_);
        if (result != VK_SUCCESS) {
            destroy_frame_image_resources(frame_images);
            return core::Result<std::size_t>::failure("renderer.vulkan_reset_fence_failed",
                                                      "failed to reset Vulkan fence: " +
                                                          std::string(vk_result_name(result)));
        }

        result = vkResetCommandBuffer(command_buffer_, 0);
        if (result != VK_SUCCESS) {
            destroy_frame_image_resources(frame_images);
            return core::Result<std::size_t>::failure("renderer.vulkan_reset_command_buffer_failed",
                                                      "failed to reset Vulkan command buffer: " +
                                                          std::string(vk_result_name(result)));
        }

        VkCommandBufferBeginInfo begin_info{};
        begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        result = vkBeginCommandBuffer(command_buffer_, &begin_info);
        if (result != VK_SUCCESS) {
            destroy_frame_image_resources(frame_images);
            return core::Result<std::size_t>::failure("renderer.vulkan_begin_command_buffer_failed",
                                                      "failed to begin Vulkan command buffer: " +
                                                          std::string(vk_result_name(result)));
        }

        VkImageLayout old_layout = offscreen_layout_;

        const VkImageSubresourceRange clear_range{
            VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1,
        };
        std::size_t submitted_barriers = 0;
        for (auto& frame_image : frame_images) {
            const auto resource_transitions =
                transitions_for_resource(frame_transitions, frame_image.name);
            submitted_barriers += record_planned_image_transition_barriers(
                command_buffer_, frame_image.image, clear_range, frame_image.layout,
                resource_transitions);
        }
        submitted_barriers += record_planned_image_transition_barriers(
            command_buffer_, offscreen_image_, clear_range, old_layout, target_transitions);

        VkPipelineStageFlags source_stage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        VkAccessFlags source_access = 0;
        if (old_layout != VK_IMAGE_LAYOUT_UNDEFINED) {
            source_stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
            source_access = VK_ACCESS_MEMORY_WRITE_BIT;
        }

        VkImageMemoryBarrier to_transfer{};
        to_transfer.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        to_transfer.srcAccessMask = source_access;
        to_transfer.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        to_transfer.oldLayout = old_layout;
        to_transfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        to_transfer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        to_transfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        to_transfer.image = offscreen_image_;
        to_transfer.subresourceRange = clear_range;
        vkCmdPipelineBarrier(command_buffer_, source_stage, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0,
                             nullptr, 0, nullptr, 1, &to_transfer);

        const VkClearColorValue vk_clear_color{
            {clear_color.red, clear_color.green, clear_color.blue, clear_color.alpha}};
        vkCmdClearColorImage(command_buffer_, offscreen_image_,
                             VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &vk_clear_color, 1,
                             &clear_range);

        VkImageMemoryBarrier to_general{};
        to_general.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        to_general.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        to_general.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;
        to_general.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        to_general.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        to_general.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        to_general.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        to_general.image = offscreen_image_;
        to_general.subresourceRange = clear_range;
        vkCmdPipelineBarrier(command_buffer_, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1,
                             &to_general);

        result = vkEndCommandBuffer(command_buffer_);
        if (result != VK_SUCCESS) {
            destroy_frame_image_resources(frame_images);
            return core::Result<std::size_t>::failure("renderer.vulkan_end_command_buffer_failed",
                                                      "failed to end Vulkan command buffer: " +
                                                          std::string(vk_result_name(result)));
        }

        VkSubmitInfo submit_info{};
        submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit_info.commandBufferCount = 1;
        submit_info.pCommandBuffers = &command_buffer_;
        result = vkQueueSubmit(queue_, 1, &submit_info, fence_);
        if (result != VK_SUCCESS) {
            destroy_frame_image_resources(frame_images);
            return core::Result<std::size_t>::failure(
                "renderer.vulkan_queue_submit_failed",
                "failed to submit Vulkan smoke command buffer: " +
                    std::string(vk_result_name(result)));
        }

        result = vkWaitForFences(device_, 1, &fence_, VK_TRUE,
                                 std::numeric_limits<std::uint64_t>::max());
        if (result != VK_SUCCESS) {
            destroy_frame_image_resources(frame_images);
            return core::Result<std::size_t>::failure("renderer.vulkan_wait_fence_failed",
                                                      "failed to wait for Vulkan smoke fence: " +
                                                          std::string(vk_result_name(result)));
        }

        offscreen_layout_ = VK_IMAGE_LAYOUT_GENERAL;
        destroy_frame_image_resources(frame_images);
        return core::Result<std::size_t>::success(submitted_barriers);
    }

    rhi::RenderDeviceDesc desc_;
    VkInstance instance_ = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT debug_messenger_ = VK_NULL_HANDLE;
    bool validation_enabled_ = false;
    bool debug_utils_enabled_ = false;
    VkPhysicalDevice physical_device_ = VK_NULL_HANDLE;
    VkPhysicalDeviceProperties properties_{};
    std::uint32_t graphics_queue_family_ = 0;
    VkFormat depth_format_ = VK_FORMAT_UNDEFINED;
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE;
    std::uint32_t timestamp_valid_bits_ = 0;
    VkQueryPool timestamp_query_pool_ = VK_NULL_HANDLE;
    std::array<bool, timestamp_slot_count> timestamp_pending_{};
    std::array<std::uint64_t, timestamp_slot_count> timestamp_frame_indices_{};
    PFN_vkCmdBeginDebugUtilsLabelEXT begin_debug_label_ = nullptr;
    PFN_vkCmdEndDebugUtilsLabelEXT end_debug_label_ = nullptr;
    VkSurfaceKHR surface_ = VK_NULL_HANDLE;
    VkCommandPool command_pool_ = VK_NULL_HANDLE;
    VkCommandBuffer command_buffer_ = VK_NULL_HANDLE;
    VkFence fence_ = VK_NULL_HANDLE;
    VkSemaphore image_available_semaphore_ = VK_NULL_HANDLE;
    VkSemaphore render_finished_semaphore_ = VK_NULL_HANDLE;
    std::vector<VulkanFrameContext> frame_contexts_;
    std::vector<VulkanUploadContext> upload_contexts_;
    std::size_t next_frame_context_ = 0;
    std::size_t next_upload_context_ = 0;
    VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
    VkFormat swapchain_format_ = VK_FORMAT_UNDEFINED;
    rhi::RenderExtent swapchain_extent_{};
    std::vector<VkImage> swapchain_images_;
    std::vector<VkImageLayout> swapchain_image_layouts_;
    std::vector<VkSemaphore> swapchain_render_finished_semaphores_;
    std::uint64_t next_resource_id_ = 1;
    std::unordered_map<std::uint64_t, VulkanBufferResource> buffer_resources_;
    VulkanBufferResource staging_buffer_;
    void* staging_mapped_ = nullptr;
    StagingRingAllocator staging_ring_{32U * 1024U * 1024U};
    std::unordered_map<std::uint64_t, VulkanImageResource> image_resources_;
    VkSampler default_sampler_ = VK_NULL_HANDLE;
    std::unordered_map<std::uint64_t, VulkanSamplerResource> sampler_resources_;
    std::unordered_map<std::uint64_t, VulkanShaderModuleResource> shader_modules_;
    std::unordered_map<std::uint64_t, VulkanComputePipelineResource> compute_pipelines_;
    std::unordered_map<std::uint64_t, VulkanGraphicsPipelineResource> graphics_pipelines_;
    std::vector<RetiredVulkanResource<VulkanBufferResource>> retired_buffers_;
    std::vector<RetiredVulkanResource<VulkanImageResource>> retired_images_;
    std::vector<RetiredVulkanResource<VulkanSamplerResource>> retired_samplers_;
    std::vector<RetiredVulkanResource<VulkanShaderModuleResource>> retired_shader_modules_;
    std::vector<RetiredVulkanResource<VulkanComputePipelineResource>> retired_compute_pipelines_;
    std::vector<RetiredVulkanResource<VulkanGraphicsPipelineResource>> retired_graphics_pipelines_;
    std::unordered_map<std::string, rhi::RenderDescriptorWrite> descriptor_write_records_;
    std::unordered_map<std::string, VulkanPipelineLayoutResource> pipeline_layouts_;
    VkImage offscreen_image_ = VK_NULL_HANDLE;
    VkImageView offscreen_image_view_ = VK_NULL_HANDLE;
    VkDeviceMemory offscreen_memory_ = VK_NULL_HANDLE;
    rhi::RenderExtent offscreen_extent_{};
    VkImageLayout offscreen_layout_ = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImage depth_image_ = VK_NULL_HANDLE;
    VkImageView depth_image_view_ = VK_NULL_HANDLE;
    VkDeviceMemory depth_memory_ = VK_NULL_HANDLE;
    VkImageLayout depth_layout_ = VK_IMAGE_LAYOUT_UNDEFINED;
    std::uint64_t completed_frame_count_ = 0;
    std::uint64_t next_frame_index_ = 0;
    std::uint64_t last_submission_serial_ = 0;
    std::uint64_t completed_submission_serial_ = 0;
    std::deque<std::uint64_t> pending_frame_submissions_;
    bool latest_gpu_upload_timing_valid_ = false;
    std::uint64_t latest_gpu_upload_submission_serial_ = 0;
    double latest_gpu_upload_ms_ = 0.0;
};

} // namespace

rhi::RendererBackendInfo backend_info() noexcept {
    if (!probe_backend()) {
        return rhi::RendererBackendInfo{
            rhi::RenderBackend::vulkan,
            rhi::render_backend_name(rhi::RenderBackend::vulkan),
            false,
            "vulkan backend is compiled but no physical device is available",
        };
    }
    return rhi::RendererBackendInfo{
        rhi::RenderBackend::vulkan,
        rhi::render_backend_name(rhi::RenderBackend::vulkan),
        true,
        "available",
    };
}

core::Result<std::unique_ptr<rhi::IRenderDevice>> create_device(rhi::RenderDeviceDesc desc) {
    auto instance = create_instance(desc);
    if (!instance) {
        return core::Result<std::unique_ptr<rhi::IRenderDevice>>::failure(instance.error().code,
                                                                          instance.error().message);
    }

    VkSurfaceKHR surface = VK_NULL_HANDLE;
    if (desc.native_window.has_value()) {
        auto native_surface =
            create_native_surface(instance.value().instance, desc.native_window.value());
        if (!native_surface) {
            destroy_instance_resource(instance.value());
            return core::Result<std::unique_ptr<rhi::IRenderDevice>>::failure(
                native_surface.error().code, native_surface.error().message);
        }
        surface = native_surface.value();
    }

    auto selected = select_physical_device(instance.value().instance, surface);
    if (!selected) {
        if (surface != VK_NULL_HANDLE) {
            vkDestroySurfaceKHR(instance.value().instance, surface, nullptr);
        }
        destroy_instance_resource(instance.value());
        return core::Result<std::unique_ptr<rhi::IRenderDevice>>::failure(selected.error().code,
                                                                          selected.error().message);
    }

    auto device =
        create_logical_device(selected.value().physical_device,
                              selected.value().graphics_queue_family, surface != VK_NULL_HANDLE);
    if (!device) {
        if (surface != VK_NULL_HANDLE) {
            vkDestroySurfaceKHR(instance.value().instance, surface, nullptr);
        }
        destroy_instance_resource(instance.value());
        return core::Result<std::unique_ptr<rhi::IRenderDevice>>::failure(device.error().code,
                                                                          device.error().message);
    }

    VkQueue queue = VK_NULL_HANDLE;
    vkGetDeviceQueue(device.value(), selected.value().graphics_queue_family, 0, &queue);
    if (queue == VK_NULL_HANDLE) {
        vkDestroyDevice(device.value(), nullptr);
        if (surface != VK_NULL_HANDLE) {
            vkDestroySurfaceKHR(instance.value().instance, surface, nullptr);
        }
        destroy_instance_resource(instance.value());
        return core::Result<std::unique_ptr<rhi::IRenderDevice>>::failure(
            "renderer.vulkan_queue_unavailable", "Vulkan logical device did not expose a queue");
    }

    auto command_pool = create_command_pool(device.value(), selected.value().graphics_queue_family);
    if (!command_pool) {
        vkDestroyDevice(device.value(), nullptr);
        if (surface != VK_NULL_HANDLE) {
            vkDestroySurfaceKHR(instance.value().instance, surface, nullptr);
        }
        destroy_instance_resource(instance.value());
        return core::Result<std::unique_ptr<rhi::IRenderDevice>>::failure(
            command_pool.error().code, command_pool.error().message);
    }

    auto command_buffer = allocate_command_buffer(device.value(), command_pool.value());
    if (!command_buffer) {
        vkDestroyCommandPool(device.value(), command_pool.value(), nullptr);
        vkDestroyDevice(device.value(), nullptr);
        if (surface != VK_NULL_HANDLE) {
            vkDestroySurfaceKHR(instance.value().instance, surface, nullptr);
        }
        destroy_instance_resource(instance.value());
        return core::Result<std::unique_ptr<rhi::IRenderDevice>>::failure(
            command_buffer.error().code, command_buffer.error().message);
    }

    auto fence = create_fence(device.value());
    if (!fence) {
        vkDestroyCommandPool(device.value(), command_pool.value(), nullptr);
        vkDestroyDevice(device.value(), nullptr);
        if (surface != VK_NULL_HANDLE) {
            vkDestroySurfaceKHR(instance.value().instance, surface, nullptr);
        }
        destroy_instance_resource(instance.value());
        return core::Result<std::unique_ptr<rhi::IRenderDevice>>::failure(fence.error().code,
                                                                          fence.error().message);
    }

    auto image_available_semaphore = create_semaphore(device.value());
    if (!image_available_semaphore) {
        vkDestroyFence(device.value(), fence.value(), nullptr);
        vkDestroyCommandPool(device.value(), command_pool.value(), nullptr);
        vkDestroyDevice(device.value(), nullptr);
        if (surface != VK_NULL_HANDLE) {
            vkDestroySurfaceKHR(instance.value().instance, surface, nullptr);
        }
        destroy_instance_resource(instance.value());
        return core::Result<std::unique_ptr<rhi::IRenderDevice>>::failure(
            image_available_semaphore.error().code, image_available_semaphore.error().message);
    }

    auto render_finished_semaphore = create_semaphore(device.value());
    if (!render_finished_semaphore) {
        vkDestroySemaphore(device.value(), image_available_semaphore.value(), nullptr);
        vkDestroyFence(device.value(), fence.value(), nullptr);
        vkDestroyCommandPool(device.value(), command_pool.value(), nullptr);
        vkDestroyDevice(device.value(), nullptr);
        if (surface != VK_NULL_HANDLE) {
            vkDestroySurfaceKHR(instance.value().instance, surface, nullptr);
        }
        destroy_instance_resource(instance.value());
        return core::Result<std::unique_ptr<rhi::IRenderDevice>>::failure(
            render_finished_semaphore.error().code, render_finished_semaphore.error().message);
    }

    auto backend = std::make_unique<VulkanSmokeDevice>(
        std::move(desc), std::move(instance).value(), std::move(selected).value(), device.value(),
        queue, surface, command_pool.value(), command_buffer.value(), fence.value(),
        image_available_semaphore.value(), render_finished_semaphore.value());
    auto frame_status = backend->initialize_frame_contexts();
    if (!frame_status) {
        return core::Result<std::unique_ptr<rhi::IRenderDevice>>::failure(
            frame_status.error().code, frame_status.error().message);
    }
    return core::Result<std::unique_ptr<rhi::IRenderDevice>>::success(std::move(backend));
}

#else

rhi::RendererBackendInfo backend_info() noexcept {
    return rhi::RendererBackendInfo{
        rhi::RenderBackend::vulkan,
        rhi::render_backend_name(rhi::RenderBackend::vulkan),
        false,
        "vulkan backend is not compiled in yet",
    };
}

core::Result<std::unique_ptr<rhi::IRenderDevice>> create_device(rhi::RenderDeviceDesc) {
    return core::Result<std::unique_ptr<rhi::IRenderDevice>>::failure(
        "renderer.vulkan_unavailable", "vulkan backend is not compiled in yet");
}

#endif

} // namespace heartstead::renderer::vulkan
