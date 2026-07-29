#include "engine/renderer/ui/ui_renderer.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <ranges>

namespace heartstead::renderer {

namespace {

[[nodiscard]] bool valid_color(const std::array<float, 4>& color) noexcept {
    return std::ranges::all_of(color, [](float value) {
        return std::isfinite(value) && value >= 0.0F && value <= 1.0F;
    });
}

} // namespace

core::Status UiRendererConfig::validate() const {
    constexpr auto maximum_size = std::numeric_limits<std::size_t>::max();
    if (maximum_vertices < 3 || maximum_indices < 3 || buffered_frames < 2 ||
        buffered_frames > 8 || atlas_layers != 2 ||
        maximum_vertices > maximum_size / buffered_frames / sizeof(GpuUiVertex) ||
        maximum_indices > maximum_size / buffered_frames / sizeof(std::uint32_t)) {
        return core::Status::failure(
            "ui_renderer.invalid_config",
            "UI capacities must fit memory, use the two-layer built-in atlas, and buffer two to "
            "eight frames");
    }
    return core::Status::ok();
}

core::Status validate_ui_vertex(const UiVertex& vertex) noexcept {
    if (!vertex.position_pixels.is_finite() || !vertex.uv.is_finite() ||
        !valid_color(vertex.color)) {
        return core::Status::failure("ui_renderer.invalid_vertex",
                                     "UI vertex position, UV, and color must be finite and valid");
    }
    return core::Status::ok();
}

core::Status validate_ui_scissor(UiScissorRect scissor, rhi::RenderExtent extent) noexcept {
    const auto right = static_cast<std::uint64_t>(scissor.x) + scissor.width;
    const auto bottom = static_cast<std::uint64_t>(scissor.y) + scissor.height;
    if (scissor.width == 0 || scissor.height == 0 || right > extent.width ||
        bottom > extent.height) {
        return core::Status::failure(
            "ui_renderer.invalid_scissor",
            "UI scissor must be nonempty and contained by the current framebuffer extent");
    }
    return core::Status::ok();
}

math::Vec2f ui_pixel_position_to_ndc(math::Vec2f position_pixels,
                                     rhi::RenderExtent extent) noexcept {
    return {
        position_pixels.x * 2.0F / static_cast<float>(extent.width) - 1.0F,
        position_pixels.y * 2.0F / static_cast<float>(extent.height) - 1.0F,
    };
}

UiRenderer::UiRenderer(rhi::IRenderDevice& device, rhi::RenderResourceHandle pipeline)
    : device_(&device), pipeline_(pipeline) {}

UiRenderer::~UiRenderer() {
    (void)shutdown();
}

core::Status UiRenderer::initialize(rhi::RenderExtent extent, UiRendererConfig config) {
    if (vertex_buffer_.is_valid() || index_buffer_.is_valid()) {
        return core::Status::failure("ui_renderer.already_initialized",
                                     "UI renderer cannot be initialized twice");
    }
    auto status = config.validate();
    if (!status) {
        return status;
    }
    status = rhi::validate_render_extent(extent);
    if (!status) {
        return status;
    }
    if (!pipeline_.is_valid()) {
        return core::Status::failure("ui_renderer.invalid_pipeline",
                                     "UI renderer requires a valid graphics pipeline");
    }
    config_ = config;
    extent_ = extent;
    const auto vertex_capacity = static_cast<std::size_t>(config_.maximum_vertices) *
                                 config_.buffered_frames;
    const auto index_capacity = static_cast<std::size_t>(config_.maximum_indices) *
                                config_.buffered_frames;
    auto vertices = device_->create_buffer({rhi::RenderBufferUsage::vertex,
                                            vertex_capacity * sizeof(GpuUiVertex),
                                            "ui_vertices",
                                            rhi::RenderBufferMemory::device_local});
    if (!vertices) {
        return core::Status::failure(vertices.error().code, vertices.error().message);
    }
    vertex_buffer_ = vertices.value().handle;
    auto indices = device_->create_buffer({rhi::RenderBufferUsage::index,
                                           index_capacity * sizeof(std::uint32_t),
                                           "ui_indices",
                                           rhi::RenderBufferMemory::device_local});
    if (!indices) {
        const auto error = indices.error();
        (void)device_->release_resource(vertex_buffer_);
        vertex_buffer_ = {};
        return core::Status::failure(error.code, error.message);
    }
    index_buffer_ = indices.value().handle;
    vertices_.reserve(config_.maximum_vertices);
    indices_.reserve(config_.maximum_indices);
    batches_.reserve(config_.maximum_indices / 3U);
    gpu_vertices_.reserve(config_.maximum_vertices);
    text_vertices_.reserve(256);
    text_indices_.reserve(384);
    return core::Status::ok();
}

core::Status UiRenderer::submit_triangles(const UiTriangleBatchDesc& batch) {
    if (!vertex_buffer_.is_valid() || !index_buffer_.is_valid()) {
        return core::Status::failure("ui_renderer.not_initialized",
                                     "UI renderer must be initialized first");
    }
    if (batch.vertices.empty() || batch.indices.empty() || batch.indices.size() % 3U != 0) {
        return core::Status::failure("ui_renderer.invalid_batch",
                                     "UI batches require vertices and triangle-list indices");
    }
    if (batch.texture_layer >= config_.atlas_layers) {
        return core::Status::failure("ui_renderer.invalid_texture_layer",
                                     "UI batch texture layer is outside the configured atlas");
    }
    if (batch.scissor_enabled) {
        auto status = validate_ui_scissor(batch.scissor, extent_);
        if (!status) {
            return status;
        }
    }
    for (const auto& vertex : batch.vertices) {
        auto status = validate_ui_vertex(vertex);
        if (!status) {
            return status;
        }
    }
    if (std::ranges::any_of(batch.indices, [&batch](std::uint32_t index) {
            return index >= batch.vertices.size();
        })) {
        return core::Status::failure("ui_renderer.index_out_of_bounds",
                                     "UI batch index references a missing vertex");
    }
    if (vertices_.size() + batch.vertices.size() > config_.maximum_vertices ||
        indices_.size() + batch.indices.size() > config_.maximum_indices) {
        ++overflowed_batches_;
        return core::Status::failure("ui_renderer.capacity_exhausted",
                                     "UI frame geometry capacity is exhausted");
    }
    PendingBatch pending;
    pending.first_vertex = static_cast<std::uint32_t>(vertices_.size());
    pending.vertex_count = static_cast<std::uint32_t>(batch.vertices.size());
    pending.first_index = static_cast<std::uint32_t>(indices_.size());
    pending.index_count = static_cast<std::uint32_t>(batch.indices.size());
    pending.texture_layer = batch.texture_layer;
    pending.scissor_enabled = batch.scissor_enabled;
    pending.scissor = batch.scissor;
    vertices_.insert(vertices_.end(), batch.vertices.begin(), batch.vertices.end());
    for (const auto index : batch.indices) {
        indices_.push_back(index + pending.first_vertex);
    }
    batches_.push_back(pending);
    return core::Status::ok();
}

core::Status UiRenderer::submit_quad(const UiQuadDesc& quad) {
    if (!quad.minimum_pixels.is_finite() || !quad.maximum_pixels.is_finite() ||
        quad.maximum_pixels.x <= quad.minimum_pixels.x ||
        quad.maximum_pixels.y <= quad.minimum_pixels.y || !quad.uv_minimum.is_finite() ||
        !quad.uv_maximum.is_finite() || !valid_color(quad.color)) {
        return core::Status::failure("ui_renderer.invalid_quad",
                                     "UI quad bounds, UVs, or color are invalid");
    }
    const std::array vertices{
        UiVertex{{quad.minimum_pixels.x, quad.minimum_pixels.y},
                 {quad.uv_minimum.x, quad.uv_minimum.y}, quad.color},
        UiVertex{{quad.maximum_pixels.x, quad.minimum_pixels.y},
                 {quad.uv_maximum.x, quad.uv_minimum.y}, quad.color},
        UiVertex{{quad.maximum_pixels.x, quad.maximum_pixels.y},
                 {quad.uv_maximum.x, quad.uv_maximum.y}, quad.color},
        UiVertex{{quad.minimum_pixels.x, quad.maximum_pixels.y},
                 {quad.uv_minimum.x, quad.uv_maximum.y}, quad.color},
    };
    constexpr std::array<std::uint32_t, 6> indices{0, 1, 2, 0, 2, 3};
    return submit_triangles({vertices, indices, quad.texture_layer, quad.scissor_enabled,
                             quad.scissor});
}

core::Status UiRenderer::submit_text(const UiTextDesc& text) {
    if (!text.position_pixels.is_finite() || text.text.empty() ||
        !std::isfinite(text.glyph_size_pixels) || text.glyph_size_pixels <= 0.0F ||
        !valid_color(text.color)) {
        return core::Status::failure("ui_renderer.invalid_text",
                                     "UI text requires finite position, size, color, and content");
    }
    if (text.text.size() > config_.maximum_vertices / 4U) {
        ++overflowed_batches_;
        return core::Status::failure("ui_renderer.capacity_exhausted",
                                     "UI text exceeds the frame vertex capacity");
    }
    text_vertices_.clear();
    text_indices_.clear();
    auto cursor = text.position_pixels;
    const auto line_start_x = cursor.x;
    std::uint32_t glyph_count = 0;
    for (const char raw_character : text.text) {
        const auto character = static_cast<unsigned char>(raw_character);
        if (character == '\n') {
            cursor.x = line_start_x;
            cursor.y += text.glyph_size_pixels;
            continue;
        }
        const auto glyph = static_cast<std::uint32_t>(character & 0x7fU);
        const auto column = glyph % 16U;
        const auto row = glyph / 16U;
        const auto uv_min = math::Vec2f{static_cast<float>(column) / 16.0F,
                                        static_cast<float>(row) / 8.0F};
        const auto uv_max = math::Vec2f{static_cast<float>(column + 1U) / 16.0F,
                                        static_cast<float>(row + 1U) / 8.0F};
        const auto first_vertex = static_cast<std::uint32_t>(text_vertices_.size());
        text_vertices_.insert(text_vertices_.end(),
                              {UiVertex{cursor, uv_min, text.color},
                               UiVertex{{cursor.x + text.glyph_size_pixels, cursor.y},
                                        {uv_max.x, uv_min.y}, text.color},
                               UiVertex{{cursor.x + text.glyph_size_pixels,
                                         cursor.y + text.glyph_size_pixels},
                                        uv_max, text.color},
                               UiVertex{{cursor.x, cursor.y + text.glyph_size_pixels},
                                        {uv_min.x, uv_max.y}, text.color}});
        text_indices_.insert(text_indices_.end(),
                             {first_vertex, first_vertex + 1U, first_vertex + 2U, first_vertex,
                              first_vertex + 2U, first_vertex + 3U});
        cursor.x += text.glyph_size_pixels * 0.75F;
        ++glyph_count;
    }
    if (glyph_count == 0) {
        return core::Status::ok();
    }
    auto status = submit_triangles({text_vertices_, text_indices_, 1, text.scissor_enabled,
                                    text.scissor});
    if (status) {
        pending_glyph_count_ += glyph_count;
    }
    return status;
}

core::Result<UiFrameCommands> UiRenderer::build_frame(UiFrameCommands scratch) {
    if (!vertex_buffer_.is_valid() || !index_buffer_.is_valid()) {
        return core::Result<UiFrameCommands>::failure(
            "ui_renderer.not_initialized", "UI renderer must be initialized first");
    }
    scratch.draws.clear();
    gpu_vertices_.clear();
    gpu_vertices_.reserve(vertices_.size());
    for (std::size_t batch_index = 0; batch_index < batches_.size(); ++batch_index) {
        const auto& batch = batches_[batch_index];
        for (std::uint32_t index = 0; index < batch.vertex_count; ++index) {
            const auto& vertex = vertices_[batch.first_vertex + index];
            const auto position = ui_pixel_position_to_ndc(vertex.position_pixels, extent_);
            gpu_vertices_.push_back({{position.x, position.y}, {vertex.uv.x, vertex.uv.y},
                                     {vertex.color[0], vertex.color[1], vertex.color[2],
                                      vertex.color[3]},
                                     batch.texture_layer, 0});
        }
    }
    const auto frame_slot = frame_number_ % config_.buffered_frames;
    const auto vertex_segment_offset = frame_slot * config_.maximum_vertices;
    const auto index_segment_offset = frame_slot * config_.maximum_indices;
    for (const auto& batch : batches_) {
        const auto same_scissor =
            !batch.scissor_enabled ||
            (!scratch.draws.empty() && scratch.draws.back().scissor.x == batch.scissor.x &&
             scratch.draws.back().scissor.y == batch.scissor.y &&
             scratch.draws.back().scissor.width == batch.scissor.width &&
             scratch.draws.back().scissor.height == batch.scissor.height);
        const auto can_merge = !scratch.draws.empty() &&
                               scratch.draws.back().scissor_enabled == batch.scissor_enabled &&
                               same_scissor &&
                               scratch.draws.back().first_index +
                                       scratch.draws.back().index_count ==
                                   index_segment_offset + batch.first_index;
        if (can_merge) {
            scratch.draws.back().index_count += batch.index_count;
            continue;
        }
        rhi::RenderDrawCommand draw;
        draw.pipeline = pipeline_;
        draw.vertex_buffer = vertex_buffer_;
        draw.index_buffer = index_buffer_;
        draw.index_count = batch.index_count;
        draw.first_index = static_cast<std::uint32_t>(index_segment_offset + batch.first_index);
        draw.vertex_offset = static_cast<std::int32_t>(vertex_segment_offset);
        draw.index_type = rhi::RenderIndexType::uint32;
        draw.scissor_enabled = batch.scissor_enabled;
        draw.scissor = {batch.scissor.x, batch.scissor.y, batch.scissor.width,
                        batch.scissor.height};
        scratch.draws.push_back(draw);
    }
    stats_ = {};
    stats_.submitted_vertices = static_cast<std::uint32_t>(vertices_.size());
    stats_.submitted_indices = static_cast<std::uint32_t>(indices_.size());
    stats_.draw_calls = static_cast<std::uint32_t>(scratch.draws.size());
    stats_.clipped_draw_calls = static_cast<std::uint32_t>(
        std::ranges::count_if(scratch.draws, [](const rhi::RenderDrawCommand& draw) {
            return draw.scissor_enabled;
        }));
    stats_.submitted_glyphs = pending_glyph_count_;
    stats_.overflowed_batches = overflowed_batches_;
    if (!gpu_vertices_.empty()) {
        const auto vertex_bytes = std::as_bytes(std::span<const GpuUiVertex>{gpu_vertices_});
        const auto index_bytes = std::as_bytes(std::span<const std::uint32_t>{indices_});
        const std::array writes{
            rhi::RenderBufferWrite{vertex_buffer_,
                                   static_cast<std::size_t>(vertex_segment_offset) *
                                       sizeof(GpuUiVertex),
                                   vertex_bytes},
            rhi::RenderBufferWrite{index_buffer_,
                                   static_cast<std::size_t>(index_segment_offset) *
                                       sizeof(std::uint32_t),
                                   index_bytes},
        };
        auto upload = device_->upload_buffer_batch(writes);
        if (!upload) {
            return core::Result<UiFrameCommands>::failure(upload.error().code,
                                                          upload.error().message);
        }
        stats_.uploaded_bytes = vertex_bytes.size() + index_bytes.size();
    }
    ++frame_number_;
    vertices_.clear();
    indices_.clear();
    batches_.clear();
    pending_glyph_count_ = 0;
    scratch.stats = stats_;
    return core::Result<UiFrameCommands>::success(std::move(scratch));
}

core::Status UiRenderer::resize(rhi::RenderExtent extent) {
    auto status = rhi::validate_render_extent(extent);
    if (!status) {
        return status;
    }
    extent_ = extent;
    return core::Status::ok();
}

core::Status UiRenderer::set_pipeline(rhi::RenderResourceHandle pipeline) noexcept {
    if (!pipeline.is_valid()) {
        return core::Status::failure("ui_renderer.invalid_pipeline",
                                     "UI graphics pipeline must be valid");
    }
    pipeline_ = pipeline;
    return core::Status::ok();
}

void UiRenderer::clear() noexcept {
    vertices_.clear();
    indices_.clear();
    batches_.clear();
    gpu_vertices_.clear();
    text_vertices_.clear();
    text_indices_.clear();
    pending_glyph_count_ = 0;
    stats_ = {};
}

core::Status UiRenderer::shutdown() {
    clear();
    frame_number_ = 0;
    auto first_failure = core::Status::ok();
    if (vertex_buffer_.is_valid()) {
        first_failure = device_->release_resource(vertex_buffer_);
        vertex_buffer_ = {};
    }
    if (index_buffer_.is_valid()) {
        auto status = device_->release_resource(index_buffer_);
        if (!status && first_failure) {
            first_failure = status;
        }
        index_buffer_ = {};
    }
    return first_failure;
}

const UiRendererStats& UiRenderer::stats() const noexcept {
    return stats_;
}

} // namespace heartstead::renderer
