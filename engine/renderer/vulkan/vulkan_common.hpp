#pragma once

#if HEARTSTEAD_HAS_VULKAN

#include "engine/core/result.hpp"
#include "engine/renderer/rhi/render_device.hpp"

#include <vulkan/vulkan.h>

#include <cstdint>
#include <string_view>

// Helpers shared by the Vulkan backend subsystems. These used to live in vulkan_backend.cpp's
// anonymous namespace, which meant anything split out of that file had to duplicate them.
namespace heartstead::renderer::vulkan::detail {

[[nodiscard]] std::string_view vk_result_name(VkResult result) noexcept;

[[nodiscard]] VkFormat vulkan_image_format(rhi::RenderImageFormat format) noexcept;

// Prefers a memory type carrying every requested property and falls back to any compatible type.
[[nodiscard]] core::Result<std::uint32_t>
find_memory_type(VkPhysicalDevice physical_device, std::uint32_t type_bits,
                 VkMemoryPropertyFlags preferred_properties);

} // namespace heartstead::renderer::vulkan::detail

#endif
