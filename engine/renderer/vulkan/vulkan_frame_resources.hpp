#pragma once

#if HEARTSTEAD_HAS_VULKAN

#include "engine/core/result.hpp"
#include "engine/renderer/rhi/render_frame_plan.hpp"

#include <vulkan/vulkan.h>

#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace heartstead::renderer::vulkan {

// Backing store for the images a frame graph declares.
//
// The previous backend could only ever hold one colour image plus one depth image, both created as
// R8G8B8A8_UNORM regardless of what the plan asked for. That made a linear HDR scene target
// impossible to allocate and limited every frame to a single attachment set. This pool keys images
// by the resource name the graph uses, honours the declared format, and keeps transient images
// alive across frames so a steady-state frame performs no allocation.
//
// Transient resources are owned here. External resources (the swapchain image, or a headless
// offscreen target) are bound per frame by the backend and never freed by the pool.
class VulkanFrameResourcePool {
  public:
    struct Image {
        std::string name;
        VkImage image = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkFormat format = VK_FORMAT_UNDEFINED;
        // Tracked across passes so the executor can derive barriers without re-deriving state.
        VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
        rhi::RenderExtent extent{};
        bool is_depth = false;
        // External images are borrowed; the pool must not destroy them.
        bool owned = false;
    };

    VulkanFrameResourcePool() = default;
    VulkanFrameResourcePool(const VulkanFrameResourcePool&) = delete;
    VulkanFrameResourcePool& operator=(const VulkanFrameResourcePool&) = delete;
    VulkanFrameResourcePool(VulkanFrameResourcePool&&) = delete;
    VulkanFrameResourcePool& operator=(VulkanFrameResourcePool&&) = delete;
    ~VulkanFrameResourcePool();

    void initialize(VkDevice device, VkPhysicalDevice physical_device) noexcept;

    // Allocates backing images for every transient resource in the plan, reusing any existing image
    // whose name, format and extent already match. Images that no longer appear, or whose
    // description changed, are destroyed.
    [[nodiscard]] core::Status
    ensure_transients(std::span<const rhi::RenderResourceDesc> resources);

    // Supplies the image backing an external resource for this frame.
    void bind_external(std::string_view name, VkImage image, VkImageView view, VkFormat format,
                       VkImageLayout layout, rhi::RenderExtent extent);

    [[nodiscard]] Image* find(std::string_view name) noexcept;
    [[nodiscard]] const Image* find(std::string_view name) const noexcept;

    // Transient layouts do not survive a frame boundary; the contents are undefined next frame.
    void reset_transient_layouts() noexcept;
    void clear_external() noexcept;
    void destroy() noexcept;

    [[nodiscard]] std::size_t transient_count() const noexcept;

  private:
    [[nodiscard]] core::Result<Image> create_transient(const rhi::RenderResourceDesc& desc);
    void destroy_image(Image& image) noexcept;

    VkDevice device_ = VK_NULL_HANDLE;
    VkPhysicalDevice physical_device_ = VK_NULL_HANDLE;
    std::vector<Image> transients_;
    std::vector<Image> externals_;
};

} // namespace heartstead::renderer::vulkan

#endif
