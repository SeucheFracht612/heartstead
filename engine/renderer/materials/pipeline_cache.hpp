#pragma once

#include "engine/core/result.hpp"
#include "engine/renderer/assets/shader_manager.hpp"
#include "engine/renderer/rhi/render_device.hpp"

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace heartstead::renderer {

enum class RenderPhase : std::uint8_t {
    sky,
    opaque_terrain,
    alpha_tested_terrain,
    transparent_terrain,
    fluid_terrain,
    static_instances,
    debug,
    ui,
};

struct GraphicsPipelineKey {
    ShaderProgramHandle shader_program;
    std::uint64_t vertex_layout = 0;
    RenderPhase render_phase = RenderPhase::opaque_terrain;
    rhi::RenderImageFormat color_format = rhi::RenderImageFormat::rgba8_unorm;
    rhi::RenderImageFormat depth_format = rhi::RenderImageFormat::d32_sfloat;
    rhi::RenderCullMode cull_mode = rhi::RenderCullMode::back;
    rhi::RenderFrontFace front_face = rhi::RenderFrontFace::counter_clockwise;
    bool depth_test = true;
    bool depth_write = true;
    rhi::RenderCompareOperation depth_compare = rhi::RenderCompareOperation::less;
    rhi::RenderBlendMode blend_mode = rhi::RenderBlendMode::disabled;
    std::uint8_t sample_count = 1;
    std::uint64_t feature_flags = 0;

    friend auto operator<=>(const GraphicsPipelineKey&, const GraphicsPipelineKey&) = default;
};

struct PipelineCacheStats {
    std::size_t resident_pipeline_count = 0;
    std::uint64_t cache_hit_count = 0;
    std::uint64_t cache_miss_count = 0;
    std::uint64_t created_pipeline_count = 0;
    std::uint64_t rebuilt_pipeline_count = 0;
    std::uint64_t failed_pipeline_count = 0;
    std::uint64_t unexpected_creation_rejection_count = 0;
};

struct PipelineFailureDiagnostic {
    GraphicsPipelineKey key;
    std::string code;
    std::string message;
};

class PipelineCache {
  public:
    PipelineCache(rhi::IRenderDevice& device, ShaderManager& shaders);
    ~PipelineCache();

    PipelineCache(const PipelineCache&) = delete;
    PipelineCache& operator=(const PipelineCache&) = delete;

    [[nodiscard]] core::Result<rhi::RenderResourceHandle>
    prewarm(GraphicsPipelineKey key, rhi::RenderPipelineLayoutDesc layout,
            rhi::RenderGraphicsPipelineDesc pipeline);
    [[nodiscard]] core::Result<rhi::RenderResourceHandle> find(GraphicsPipelineKey key);
    [[nodiscard]] core::Status rebuild_program(ShaderProgramHandle program);
    [[nodiscard]] core::Status shutdown();

    void seal() noexcept;
    void unseal_for_development() noexcept;
    [[nodiscard]] bool is_sealed() const noexcept;
    [[nodiscard]] const PipelineCacheStats& stats() const noexcept;
    [[nodiscard]] const std::vector<PipelineFailureDiagnostic>& failures() const noexcept;

  private:
    struct Entry;

    [[nodiscard]] Entry* find_entry(const GraphicsPipelineKey& key) noexcept;
    [[nodiscard]] const Entry* find_entry(const GraphicsPipelineKey& key) const noexcept;
    [[nodiscard]] core::Result<rhi::RenderResourceHandle>
    build_pipeline(const GraphicsPipelineKey& key, const rhi::RenderPipelineLayoutDesc& layout,
                   rhi::RenderGraphicsPipelineDesc pipeline);
    void record_failure(const GraphicsPipelineKey& key, const core::Error& error);

    rhi::IRenderDevice& device_;
    ShaderManager& shaders_;
    std::vector<Entry> entries_;
    std::vector<PipelineFailureDiagnostic> failures_;
    PipelineCacheStats stats_{};
    bool sealed_ = false;
};

[[nodiscard]] std::uint64_t
hash_vertex_layout(std::uint32_t stride,
                   std::span<const rhi::RenderVertexAttributeDesc> attributes) noexcept;
[[nodiscard]] std::string_view render_phase_name(RenderPhase phase) noexcept;

} // namespace heartstead::renderer
