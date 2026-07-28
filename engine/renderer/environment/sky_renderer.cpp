#include "engine/renderer/environment/sky_renderer.hpp"

#include <array>
#include <span>

namespace heartstead::renderer {

SkyRenderer::SkyRenderer(rhi::IRenderDevice& device, rhi::RenderResourceHandle pipeline)
    : device_(&device), pipeline_(pipeline) {}

SkyRenderer::~SkyRenderer() {
    (void)shutdown();
}

core::Status SkyRenderer::initialize() {
    if (is_initialized()) {
        return core::Status::failure("sky_renderer.already_initialized",
                                     "sky renderer cannot be initialized twice");
    }
    if (device_ == nullptr || !pipeline_.is_valid()) {
        return core::Status::failure("sky_renderer.invalid_pipeline",
                                     "sky renderer requires a device and pipeline");
    }
    constexpr std::array vertices{
        GpuSkyVertex{{-1.0F, -1.0F}},
        GpuSkyVertex{{3.0F, -1.0F}},
        GpuSkyVertex{{-1.0F, 3.0F}},
    };
    constexpr std::array<std::uint16_t, 3> indices{0, 1, 2};
    auto vertex_upload =
        device_->upload_buffer({rhi::RenderBufferUsage::vertex, sizeof(vertices),
                                "sky_gradient_vertices", rhi::RenderBufferMemory::device_local},
                               std::as_bytes(std::span{vertices}));
    if (!vertex_upload) {
        return core::Status::failure(vertex_upload.error().code, vertex_upload.error().message);
    }
    vertex_buffer_ = vertex_upload.value().handle;
    auto index_upload =
        device_->upload_buffer({rhi::RenderBufferUsage::index, sizeof(indices),
                                "sky_gradient_indices", rhi::RenderBufferMemory::device_local},
                               std::as_bytes(std::span{indices}));
    if (!index_upload) {
        const auto error = index_upload.error();
        (void)device_->release_resource(vertex_buffer_);
        vertex_buffer_ = {};
        return core::Status::failure(error.code, error.message);
    }
    index_buffer_ = index_upload.value().handle;
    return core::Status::ok();
}

core::Status SkyRenderer::shutdown() {
    auto result = core::Status::ok();
    if (device_ != nullptr && index_buffer_.is_valid()) {
        result = device_->release_resource(index_buffer_);
    }
    if (device_ != nullptr && vertex_buffer_.is_valid()) {
        auto status = device_->release_resource(vertex_buffer_);
        if (!status && result) {
            result = status;
        }
    }
    index_buffer_ = {};
    vertex_buffer_ = {};
    return result;
}

core::Result<rhi::RenderDrawCommand> SkyRenderer::build_draw() const {
    if (!is_initialized()) {
        return core::Result<rhi::RenderDrawCommand>::failure(
            "sky_renderer.not_initialized", "sky renderer must be initialized before drawing");
    }
    rhi::RenderDrawCommand draw;
    draw.pipeline = pipeline_;
    draw.vertex_buffer = vertex_buffer_;
    draw.index_buffer = index_buffer_;
    draw.index_count = 3;
    draw.index_type = rhi::RenderIndexType::uint16;
    return core::Result<rhi::RenderDrawCommand>::success(draw);
}

bool SkyRenderer::is_initialized() const noexcept {
    return device_ != nullptr && pipeline_.is_valid() && vertex_buffer_.is_valid() &&
           index_buffer_.is_valid();
}

} // namespace heartstead::renderer
