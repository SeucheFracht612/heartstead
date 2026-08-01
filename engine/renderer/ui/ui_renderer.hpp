#pragma once

#include "engine/core/result.hpp"
#include "engine/math/vector.hpp"
#include "engine/renderer/rhi/render_frame_plan.hpp"
#include "engine/renderer/ui/ui_font.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

namespace heartstead::renderer {

// GPU ABI. Locations: 0 position, 1 UV, 2 color, 3 atlas layer, 4 sampling flags.
struct GpuUiVertex {
    float position[2]{};
    float uv[2]{};
    float color[4]{1.0F, 1.0F, 1.0F, 1.0F};
    std::uint16_t texture_layer = 0;
    std::uint16_t flags = 0;
};

static_assert(sizeof(GpuUiVertex) == 36);
static_assert(offsetof(GpuUiVertex, position) == 0);
static_assert(offsetof(GpuUiVertex, uv) == 8);
static_assert(offsetof(GpuUiVertex, color) == 16);
static_assert(offsetof(GpuUiVertex, texture_layer) == 32);

inline constexpr std::array<rhi::RenderVertexAttributeDesc, 5> gpu_ui_vertex_attributes{{
    {0, offsetof(GpuUiVertex, position), rhi::RenderVertexAttributeFormat::float2},
    {1, offsetof(GpuUiVertex, uv), rhi::RenderVertexAttributeFormat::float2},
    {2, offsetof(GpuUiVertex, color), rhi::RenderVertexAttributeFormat::float4},
    {3, offsetof(GpuUiVertex, texture_layer), rhi::RenderVertexAttributeFormat::uint16},
    {4, offsetof(GpuUiVertex, flags), rhi::RenderVertexAttributeFormat::uint16},
}};

struct UiVertex {
    math::Vec2f position_pixels{};
    math::Vec2f uv{};
    std::array<float, 4> color{1.0F, 1.0F, 1.0F, 1.0F};
};

struct UiScissorRect {
    std::uint32_t x = 0;
    std::uint32_t y = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;

    friend bool operator==(const UiScissorRect&, const UiScissorRect&) = default;
};

struct UiTriangleBatchDesc {
    std::span<const UiVertex> vertices;
    std::span<const std::uint32_t> indices;
    std::uint16_t texture_layer = 0;
    bool scissor_enabled = false;
    UiScissorRect scissor{};
    bool distance_field = false;
};

struct UiQuadDesc {
    math::Vec2f minimum_pixels{};
    math::Vec2f maximum_pixels{};
    math::Vec2f uv_minimum{};
    math::Vec2f uv_maximum{1.0F, 1.0F};
    std::array<float, 4> color{1.0F, 1.0F, 1.0F, 1.0F};
    std::uint16_t texture_layer = 0;
    bool scissor_enabled = false;
    UiScissorRect scissor{};
};

struct UiTextDesc {
    math::Vec2f position_pixels{};
    std::string_view text;
    float glyph_size_pixels = 8.0F;
    std::array<float, 4> color{1.0F, 1.0F, 1.0F, 1.0F};
    bool scissor_enabled = false;
    UiScissorRect scissor{};
};

struct UiRendererConfig {
    std::uint32_t maximum_vertices = 65'536;
    std::uint32_t maximum_indices = 98'304;
    std::uint32_t buffered_frames = 3;
    std::uint16_t atlas_layers = 2;

    [[nodiscard]] core::Status validate() const;
};

enum class UiColorVisionMode : std::uint8_t { none, protanopia, deuteranopia, tritanopia };

struct UiAccessibilitySettings {
    float contrast = 1.0F;
    float saturation = 1.0F;
    UiColorVisionMode color_vision_mode = UiColorVisionMode::none;

    [[nodiscard]] core::Status validate() const noexcept;
};

struct UiRendererStats {
    std::uint32_t submitted_vertices = 0;
    std::uint32_t submitted_indices = 0;
    std::uint32_t draw_calls = 0;
    std::uint32_t clipped_draw_calls = 0;
    std::uint32_t submitted_glyphs = 0;
    std::uint64_t uploaded_bytes = 0;
    std::uint64_t overflowed_batches = 0;
};

struct UiFrameCommands {
    std::vector<rhi::RenderDrawCommand> draws;
    UiRendererStats stats;
};

class UiRenderer {
  public:
    UiRenderer(rhi::IRenderDevice& device, rhi::RenderResourceHandle pipeline,
               std::shared_ptr<const UiFont> font = {});
    ~UiRenderer();

    UiRenderer(const UiRenderer&) = delete;
    UiRenderer& operator=(const UiRenderer&) = delete;

    [[nodiscard]] core::Status initialize(rhi::RenderExtent extent,
                                          UiRendererConfig config = {});
    [[nodiscard]] core::Status submit_triangles(const UiTriangleBatchDesc& batch);
    [[nodiscard]] core::Status submit_quad(const UiQuadDesc& quad);
    [[nodiscard]] core::Status submit_text(const UiTextDesc& text);
    [[nodiscard]] core::Result<UiFrameCommands> build_frame(UiFrameCommands scratch = {});
    [[nodiscard]] core::Status resize(rhi::RenderExtent extent);
    [[nodiscard]] core::Status set_pipeline(rhi::RenderResourceHandle pipeline) noexcept;
    [[nodiscard]] core::Status set_accessibility_settings(UiAccessibilitySettings settings);
    [[nodiscard]] UiAccessibilitySettings accessibility_settings() const noexcept;
    void clear() noexcept;
    [[nodiscard]] core::Status shutdown();
    [[nodiscard]] const UiRendererStats& stats() const noexcept;

  private:
    struct PendingBatch {
        std::uint32_t first_vertex = 0;
        std::uint32_t vertex_count = 0;
        std::uint32_t first_index = 0;
        std::uint32_t index_count = 0;
        std::uint16_t texture_layer = 0;
        bool scissor_enabled = false;
        UiScissorRect scissor{};
        bool distance_field = false;
    };

    rhi::IRenderDevice* device_ = nullptr;
    rhi::RenderResourceHandle pipeline_;
    std::shared_ptr<const UiFont> font_;
    rhi::RenderResourceHandle vertex_buffer_;
    rhi::RenderResourceHandle index_buffer_;
    rhi::RenderExtent extent_{};
    UiRendererConfig config_{};
    UiAccessibilitySettings accessibility_{};
    std::vector<UiVertex> vertices_;
    std::vector<std::uint32_t> indices_;
    std::vector<PendingBatch> batches_;
    std::vector<GpuUiVertex> gpu_vertices_;
    std::vector<UiVertex> text_vertices_;
    std::vector<std::uint32_t> text_indices_;
    std::uint64_t frame_number_ = 0;
    std::uint64_t overflowed_batches_ = 0;
    std::uint32_t pending_glyph_count_ = 0;
    UiRendererStats stats_{};
};

[[nodiscard]] core::Status validate_ui_vertex(const UiVertex& vertex) noexcept;
[[nodiscard]] core::Status validate_ui_scissor(UiScissorRect scissor,
                                               rhi::RenderExtent extent) noexcept;
// UI pixel coordinates use a top-left origin. The renderer's positive-height viewport maps
// Vulkan NDC (-1, -1) to that same top-left corner.
[[nodiscard]] math::Vec2f ui_pixel_position_to_ndc(math::Vec2f position_pixels,
                                                   rhi::RenderExtent extent) noexcept;
[[nodiscard]] std::array<float, 4>
apply_ui_accessibility_color(std::array<float, 4> color,
                             UiAccessibilitySettings settings) noexcept;

} // namespace heartstead::renderer
