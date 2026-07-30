#include "engine/renderer/vulkan/vulkan_frame_resources.hpp"

#if HEARTSTEAD_HAS_VULKAN

#include "engine/renderer/vulkan/vulkan_common.hpp"

#include <algorithm>
#include <utility>

namespace heartstead::renderer::vulkan {

namespace {

// Colour targets are sampled by later passes (tone mapping reads the scene target) and may be
// blitted or read back, so every transient carries the usage a graph pass can ask of it. Depth is
// sampled too, which shadow and depth-aware passes will need.
[[nodiscard]] VkImageUsageFlags transient_usage(bool is_depth) noexcept {
    if (is_depth) {
        return VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    }
    return VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
           VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
}

[[nodiscard]] VkImageAspectFlags aspect_for(bool is_depth) noexcept {
    return is_depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
}

[[nodiscard]] bool matches(const VulkanFrameResourcePool::Image& image,
                           const rhi::RenderResourceDesc& desc, VkFormat format) noexcept {
    return image.name == desc.name && image.format == format &&
           image.extent.width == desc.extent.width && image.extent.height == desc.extent.height;
}

} // namespace

VulkanFrameResourcePool::~VulkanFrameResourcePool() {
    destroy();
}

void VulkanFrameResourcePool::initialize(VkDevice device,
                                         VkPhysicalDevice physical_device) noexcept {
    device_ = device;
    physical_device_ = physical_device;
}

core::Result<VulkanFrameResourcePool::Image>
VulkanFrameResourcePool::create_transient(const rhi::RenderResourceDesc& desc) {
    Image created;
    created.name = desc.name;
    created.format = detail::vulkan_image_format(desc.format);
    created.extent = desc.extent;
    created.is_depth = rhi::is_depth_format(desc.format);
    created.owned = true;

    if (created.format == VK_FORMAT_UNDEFINED) {
        return core::Result<Image>::failure(
            "renderer.vulkan_frame_resource_format_unsupported",
            "frame graph resource '" + desc.name + "' uses a format the Vulkan backend cannot map");
    }

    VkImageCreateInfo image_info{};
    image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    image_info.imageType = VK_IMAGE_TYPE_2D;
    image_info.format = created.format;
    image_info.extent = VkExtent3D{desc.extent.width, desc.extent.height, 1};
    image_info.mipLevels = 1;
    image_info.arrayLayers = 1;
    image_info.samples = VK_SAMPLE_COUNT_1_BIT;
    image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    image_info.usage = transient_usage(created.is_depth);
    image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    auto result = vkCreateImage(device_, &image_info, nullptr, &created.image);
    if (result != VK_SUCCESS) {
        return core::Result<Image>::failure(
            "renderer.vulkan_frame_resource_image_failed",
            "failed to create Vulkan image for frame graph resource '" + desc.name +
                "': " + std::string(detail::vk_result_name(result)));
    }

    VkMemoryRequirements requirements{};
    vkGetImageMemoryRequirements(device_, created.image, &requirements);
    auto memory_type = detail::find_memory_type(physical_device_, requirements.memoryTypeBits,
                                                VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (!memory_type) {
        destroy_image(created);
        return core::Result<Image>::failure(memory_type.error().code, memory_type.error().message);
    }

    VkMemoryAllocateInfo allocation_info{};
    allocation_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocation_info.allocationSize = requirements.size;
    allocation_info.memoryTypeIndex = memory_type.value();
    result = vkAllocateMemory(device_, &allocation_info, nullptr, &created.memory);
    if (result != VK_SUCCESS) {
        destroy_image(created);
        return core::Result<Image>::failure(
            "renderer.vulkan_frame_resource_memory_failed",
            "failed to allocate memory for frame graph resource '" + desc.name +
                "': " + std::string(detail::vk_result_name(result)));
    }

    result = vkBindImageMemory(device_, created.image, created.memory, 0);
    if (result != VK_SUCCESS) {
        destroy_image(created);
        return core::Result<Image>::failure(
            "renderer.vulkan_frame_resource_bind_failed",
            "failed to bind memory for frame graph resource '" + desc.name +
                "': " + std::string(detail::vk_result_name(result)));
    }

    VkImageViewCreateInfo view_info{};
    view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view_info.image = created.image;
    view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view_info.format = created.format;
    view_info.subresourceRange.aspectMask = aspect_for(created.is_depth);
    view_info.subresourceRange.levelCount = 1;
    view_info.subresourceRange.layerCount = 1;

    result = vkCreateImageView(device_, &view_info, nullptr, &created.view);
    if (result != VK_SUCCESS) {
        destroy_image(created);
        return core::Result<Image>::failure(
            "renderer.vulkan_frame_resource_view_failed",
            "failed to create image view for frame graph resource '" + desc.name +
                "': " + std::string(detail::vk_result_name(result)));
    }

    return core::Result<Image>::success(std::move(created));
}

core::Status
VulkanFrameResourcePool::ensure_transients(std::span<const rhi::RenderResourceDesc> resources) {
    if (device_ == VK_NULL_HANDLE) {
        return core::Status::failure("renderer.vulkan_frame_resource_pool_uninitialized",
                                     "frame resource pool used before initialization");
    }

    std::vector<Image> retained;
    retained.reserve(resources.size());

    for (const auto& desc : resources) {
        if (desc.lifetime != rhi::RenderResourceLifetime::transient) {
            continue;
        }
        const auto format = detail::vulkan_image_format(desc.format);
        const auto existing = std::ranges::find_if(
            transients_, [&desc, format](const Image& candidate) {
                return candidate.image != VK_NULL_HANDLE && matches(candidate, desc, format);
            });
        if (existing != transients_.end()) {
            retained.push_back(std::move(*existing));
            // Contents do not carry across frames; passes must clear or fully overwrite.
            retained.back().layout = VK_IMAGE_LAYOUT_UNDEFINED;
            existing->image = VK_NULL_HANDLE;
            existing->view = VK_NULL_HANDLE;
            existing->memory = VK_NULL_HANDLE;
            continue;
        }
        auto created = create_transient(desc);
        if (!created) {
            for (auto& image : retained) {
                destroy_image(image);
            }
            return core::Status::failure(created.error().code, created.error().message);
        }
        retained.push_back(std::move(created).value());
    }

    // Anything not carried over is stale: a resized target, a changed format, or a resource the
    // current graph no longer declares.
    for (auto& image : transients_) {
        destroy_image(image);
    }
    transients_ = std::move(retained);
    return core::Status::ok();
}

void VulkanFrameResourcePool::bind_external(std::string_view name, VkImage image, VkImageView view,
                                            VkFormat format, VkImageLayout layout,
                                            rhi::RenderExtent extent) {
    const auto existing =
        std::ranges::find_if(externals_, [name](const Image& candidate) noexcept {
            return candidate.name == name;
        });
    Image& slot = existing != externals_.end() ? *existing : externals_.emplace_back();
    slot.name = std::string(name);
    slot.image = image;
    slot.view = view;
    slot.memory = VK_NULL_HANDLE;
    slot.format = format;
    slot.layout = layout;
    slot.extent = extent;
    slot.is_depth = false;
    slot.owned = false;
}

VulkanFrameResourcePool::Image* VulkanFrameResourcePool::find(std::string_view name) noexcept {
    const auto external = std::ranges::find_if(
        externals_, [name](const Image& candidate) noexcept { return candidate.name == name; });
    if (external != externals_.end()) {
        return &*external;
    }
    const auto transient = std::ranges::find_if(
        transients_, [name](const Image& candidate) noexcept { return candidate.name == name; });
    return transient != transients_.end() ? &*transient : nullptr;
}

const VulkanFrameResourcePool::Image*
VulkanFrameResourcePool::find(std::string_view name) const noexcept {
    return const_cast<VulkanFrameResourcePool*>(this)->find(name);
}

void VulkanFrameResourcePool::reset_transient_layouts() noexcept {
    for (auto& image : transients_) {
        image.layout = VK_IMAGE_LAYOUT_UNDEFINED;
    }
}

void VulkanFrameResourcePool::clear_external() noexcept {
    externals_.clear();
}

void VulkanFrameResourcePool::destroy_image(Image& image) noexcept {
    if (!image.owned) {
        image.image = VK_NULL_HANDLE;
        image.view = VK_NULL_HANDLE;
        image.memory = VK_NULL_HANDLE;
        return;
    }
    if (image.view != VK_NULL_HANDLE) {
        vkDestroyImageView(device_, image.view, nullptr);
        image.view = VK_NULL_HANDLE;
    }
    if (image.image != VK_NULL_HANDLE) {
        vkDestroyImage(device_, image.image, nullptr);
        image.image = VK_NULL_HANDLE;
    }
    if (image.memory != VK_NULL_HANDLE) {
        vkFreeMemory(device_, image.memory, nullptr);
        image.memory = VK_NULL_HANDLE;
    }
    image.layout = VK_IMAGE_LAYOUT_UNDEFINED;
}

void VulkanFrameResourcePool::destroy() noexcept {
    for (auto& image : transients_) {
        destroy_image(image);
    }
    transients_.clear();
    externals_.clear();
}

std::size_t VulkanFrameResourcePool::transient_count() const noexcept {
    return transients_.size();
}

} // namespace heartstead::renderer::vulkan

#endif
