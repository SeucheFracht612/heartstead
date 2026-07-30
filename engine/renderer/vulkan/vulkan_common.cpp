#include "engine/renderer/vulkan/vulkan_common.hpp"

#if HEARTSTEAD_HAS_VULKAN

namespace heartstead::renderer::vulkan::detail {

std::string_view vk_result_name(VkResult result) noexcept {
    switch (result) {
    case VK_SUCCESS:
        return "VK_SUCCESS";
    case VK_NOT_READY:
        return "VK_NOT_READY";
    case VK_TIMEOUT:
        return "VK_TIMEOUT";
    case VK_EVENT_SET:
        return "VK_EVENT_SET";
    case VK_EVENT_RESET:
        return "VK_EVENT_RESET";
    case VK_INCOMPLETE:
        return "VK_INCOMPLETE";
    case VK_ERROR_OUT_OF_HOST_MEMORY:
        return "VK_ERROR_OUT_OF_HOST_MEMORY";
    case VK_ERROR_OUT_OF_DEVICE_MEMORY:
        return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
    case VK_ERROR_INITIALIZATION_FAILED:
        return "VK_ERROR_INITIALIZATION_FAILED";
    case VK_ERROR_DEVICE_LOST:
        return "VK_ERROR_DEVICE_LOST";
    case VK_ERROR_MEMORY_MAP_FAILED:
        return "VK_ERROR_MEMORY_MAP_FAILED";
    case VK_ERROR_LAYER_NOT_PRESENT:
        return "VK_ERROR_LAYER_NOT_PRESENT";
    case VK_ERROR_EXTENSION_NOT_PRESENT:
        return "VK_ERROR_EXTENSION_NOT_PRESENT";
    case VK_ERROR_FEATURE_NOT_PRESENT:
        return "VK_ERROR_FEATURE_NOT_PRESENT";
    case VK_ERROR_INCOMPATIBLE_DRIVER:
        return "VK_ERROR_INCOMPATIBLE_DRIVER";
    case VK_ERROR_TOO_MANY_OBJECTS:
        return "VK_ERROR_TOO_MANY_OBJECTS";
    case VK_ERROR_FORMAT_NOT_SUPPORTED:
        return "VK_ERROR_FORMAT_NOT_SUPPORTED";
    case VK_ERROR_FRAGMENTED_POOL:
        return "VK_ERROR_FRAGMENTED_POOL";
    case VK_ERROR_OUT_OF_DATE_KHR:
        return "VK_ERROR_OUT_OF_DATE_KHR";
    case VK_SUBOPTIMAL_KHR:
        return "VK_SUBOPTIMAL_KHR";
    default:
        return "VK_UNKNOWN_RESULT";
    }
}

VkFormat vulkan_image_format(rhi::RenderImageFormat format) noexcept {
    switch (format) {
    case rhi::RenderImageFormat::r8_unorm:
        return VK_FORMAT_R8_UNORM;
    case rhi::RenderImageFormat::rg16_sfloat:
        return VK_FORMAT_R16G16_SFLOAT;
    case rhi::RenderImageFormat::rgba8_unorm:
        return VK_FORMAT_R8G8B8A8_UNORM;
    case rhi::RenderImageFormat::rgba8_srgb:
        return VK_FORMAT_R8G8B8A8_SRGB;
    case rhi::RenderImageFormat::rgba16_sfloat:
        return VK_FORMAT_R16G16B16A16_SFLOAT;
    case rhi::RenderImageFormat::bc1_rgb_unorm:
        return VK_FORMAT_BC1_RGB_UNORM_BLOCK;
    case rhi::RenderImageFormat::bc1_rgb_srgb:
        return VK_FORMAT_BC1_RGB_SRGB_BLOCK;
    case rhi::RenderImageFormat::bc3_rgba_unorm:
        return VK_FORMAT_BC3_UNORM_BLOCK;
    case rhi::RenderImageFormat::bc3_rgba_srgb:
        return VK_FORMAT_BC3_SRGB_BLOCK;
    case rhi::RenderImageFormat::bc5_rg_unorm:
        return VK_FORMAT_BC5_UNORM_BLOCK;
    case rhi::RenderImageFormat::bc7_rgba_unorm:
        return VK_FORMAT_BC7_UNORM_BLOCK;
    case rhi::RenderImageFormat::bc7_rgba_srgb:
        return VK_FORMAT_BC7_SRGB_BLOCK;
    case rhi::RenderImageFormat::d32_sfloat:
        return VK_FORMAT_D32_SFLOAT;
    case rhi::RenderImageFormat::d32_sfloat_s8_uint:
        return VK_FORMAT_D32_SFLOAT_S8_UINT;
    case rhi::RenderImageFormat::d24_unorm_s8_uint:
        return VK_FORMAT_D24_UNORM_S8_UINT;
    }
    return VK_FORMAT_UNDEFINED;
}

core::Result<std::uint32_t> find_memory_type(VkPhysicalDevice physical_device,
                                             std::uint32_t type_bits,
                                             VkMemoryPropertyFlags preferred_properties) {
    VkPhysicalDeviceMemoryProperties memory_properties{};
    vkGetPhysicalDeviceMemoryProperties(physical_device, &memory_properties);

    for (std::uint32_t index = 0; index < memory_properties.memoryTypeCount; ++index) {
        const auto type_supported = (type_bits & (1U << index)) != 0;
        const auto has_properties = (memory_properties.memoryTypes[index].propertyFlags &
                                     preferred_properties) == preferred_properties;
        if (type_supported && has_properties) {
            return core::Result<std::uint32_t>::success(index);
        }
    }

    for (std::uint32_t index = 0; index < memory_properties.memoryTypeCount; ++index) {
        if ((type_bits & (1U << index)) != 0) {
            return core::Result<std::uint32_t>::success(index);
        }
    }

    return core::Result<std::uint32_t>::failure(
        "renderer.vulkan_memory_type_unavailable",
        "no compatible Vulkan memory type is available for the requested allocation");
}

} // namespace heartstead::renderer::vulkan::detail

#endif
