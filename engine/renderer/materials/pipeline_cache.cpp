#include "engine/renderer/materials/pipeline_cache.hpp"

#include "engine/core/hash.hpp"

#include <algorithm>
#include <array>
#include <ranges>
#include <utility>

namespace heartstead::renderer {

struct PipelineCache::Entry {
    GraphicsPipelineKey key;
    rhi::RenderPipelineLayoutDesc layout;
    rhi::RenderGraphicsPipelineDesc pipeline_desc;
    rhi::RenderResourceHandle pipeline;
    std::uint64_t shader_revision = 0;
};

PipelineCache::PipelineCache(rhi::IRenderDevice& device, ShaderManager& shaders)
    : device_(device), shaders_(shaders) {}

PipelineCache::~PipelineCache() {
    (void)shutdown();
}

core::Result<rhi::RenderResourceHandle>
PipelineCache::prewarm(GraphicsPipelineKey key, rhi::RenderPipelineLayoutDesc layout,
                       rhi::RenderGraphicsPipelineDesc pipeline) {
    if (!key.shader_program.is_valid()) {
        return core::Result<rhi::RenderResourceHandle>::failure(
            "pipeline_cache.invalid_shader_program", "pipeline key requires a shader program");
    }
    if (auto* existing = find_entry(key); existing != nullptr) {
        ++stats_.cache_hit_count;
        return core::Result<rhi::RenderResourceHandle>::success(existing->pipeline);
    }
    ++stats_.cache_miss_count;
    if (sealed_) {
        ++stats_.unexpected_creation_rejection_count;
        return core::Result<rhi::RenderResourceHandle>::failure(
            "pipeline_cache.runtime_creation_rejected",
            "pipeline cache is sealed; pipelines must be prewarmed before frame rendering");
    }
    auto built = build_pipeline(key, layout, pipeline);
    if (!built) {
        record_failure(key, built.error());
        return built;
    }
    const auto* program = shaders_.find(key.shader_program);
    entries_.push_back(Entry{std::move(key), std::move(layout), std::move(pipeline), built.value(),
                             program->revision});
    stats_.resident_pipeline_count = entries_.size();
    stats_.resident_pipeline_layout_count = entries_.size();
    stats_.descriptor_binding_count += entries_.back().layout.descriptors.size();
    ++stats_.created_pipeline_count;
    return built;
}

core::Result<rhi::RenderResourceHandle> PipelineCache::find(GraphicsPipelineKey key) {
    auto* entry = find_entry(key);
    if (entry == nullptr) {
        ++stats_.cache_miss_count;
        return core::Result<rhi::RenderResourceHandle>::failure("pipeline_cache.missing_pipeline",
                                                                "pipeline key was not prewarmed");
    }
    ++stats_.cache_hit_count;
    return core::Result<rhi::RenderResourceHandle>::success(entry->pipeline);
}

core::Status PipelineCache::rebuild_program(ShaderProgramHandle program_handle) {
    const auto* program = shaders_.find(program_handle);
    if (program == nullptr) {
        return core::Status::failure("pipeline_cache.stale_shader_program",
                                     "pipeline rebuild references a stale shader program");
    }

    struct Replacement {
        std::size_t entry_index = 0;
        rhi::RenderResourceHandle pipeline;
    };
    std::vector<Replacement> replacements;
    for (std::size_t index = 0; index < entries_.size(); ++index) {
        auto& entry = entries_[index];
        if (entry.key.shader_program != program_handle ||
            entry.shader_revision == program->revision) {
            continue;
        }
        auto built = build_pipeline(entry.key, entry.layout, entry.pipeline_desc);
        if (!built) {
            for (const auto& replacement : replacements) {
                (void)device_.release_resource(replacement.pipeline);
            }
            record_failure(entry.key, built.error());
            return core::Status::failure(built.error().code, built.error().message);
        }
        replacements.push_back({index, built.value()});
    }

    for (const auto& replacement : replacements) {
        auto& entry = entries_[replacement.entry_index];
        auto status = device_.release_resource(entry.pipeline);
        if (!status) {
            return status;
        }
        entry.pipeline = replacement.pipeline;
        entry.shader_revision = program->revision;
        ++stats_.rebuilt_pipeline_count;
    }
    return shaders_.release_superseded_modules(program_handle);
}

core::Status PipelineCache::shutdown() {
    core::Status first_failure = core::Status::ok();
    for (auto& entry : entries_) {
        if (!entry.pipeline.is_valid()) {
            continue;
        }
        auto status = device_.release_resource(entry.pipeline);
        if (!status && first_failure) {
            first_failure = status;
        }
        entry.pipeline = {};
    }
    entries_.clear();
    stats_.resident_pipeline_count = 0;
    stats_.resident_pipeline_layout_count = 0;
    stats_.descriptor_binding_count = 0;
    return first_failure;
}

void PipelineCache::seal() noexcept {
    sealed_ = true;
}

void PipelineCache::unseal_for_development() noexcept {
    sealed_ = false;
}

bool PipelineCache::is_sealed() const noexcept {
    return sealed_;
}

const PipelineCacheStats& PipelineCache::stats() const noexcept {
    return stats_;
}

const std::vector<PipelineFailureDiagnostic>& PipelineCache::failures() const noexcept {
    return failures_;
}

PipelineCache::Entry* PipelineCache::find_entry(const GraphicsPipelineKey& key) noexcept {
    const auto found =
        std::ranges::find_if(entries_, [&key](const Entry& entry) { return entry.key == key; });
    return found == entries_.end() ? nullptr : &*found;
}

const PipelineCache::Entry*
PipelineCache::find_entry(const GraphicsPipelineKey& key) const noexcept {
    const auto found =
        std::ranges::find_if(entries_, [&key](const Entry& entry) { return entry.key == key; });
    return found == entries_.end() ? nullptr : &*found;
}

core::Result<rhi::RenderResourceHandle>
PipelineCache::build_pipeline(const GraphicsPipelineKey& key,
                              const rhi::RenderPipelineLayoutDesc& layout,
                              rhi::RenderGraphicsPipelineDesc pipeline) {
    const auto* program = shaders_.find(key.shader_program);
    if (program == nullptr) {
        return core::Result<rhi::RenderResourceHandle>::failure(
            "pipeline_cache.stale_shader_program", "pipeline references a stale shader program");
    }
    auto interface_status = validate_shader_interface(*program, layout, pipeline.vertex_stride,
                                                      pipeline.vertex_attributes);
    if (!interface_status) {
        return core::Result<rhi::RenderResourceHandle>::failure(interface_status.error().code,
                                                                interface_status.error().message);
    }
    if (hash_vertex_layout(pipeline.vertex_stride, pipeline.vertex_attributes) !=
        key.vertex_layout) {
        return core::Result<rhi::RenderResourceHandle>::failure(
            "pipeline_cache.vertex_layout_key_mismatch",
            "pipeline descriptor vertex layout does not match its cache key");
    }
    if (pipeline.color_target_format != key.color_format ||
        pipeline.depth_target_format != key.depth_format || pipeline.cull_mode != key.cull_mode ||
        pipeline.front_face != key.front_face || pipeline.depth_test_enable != key.depth_test ||
        pipeline.depth_write_enable != key.depth_write ||
        pipeline.depth_compare != key.depth_compare || pipeline.blend_mode != key.blend_mode ||
        pipeline.color_write_enable != key.color_write ||
        pipeline.additional_color_target_formats != key.additional_color_formats) {
        return core::Result<rhi::RenderResourceHandle>::failure(
            "pipeline_cache.state_key_mismatch",
            "pipeline render state does not match its cache key");
    }

    // Binding an identical layout is expected during explicit prewarm/reload, never per frame.
    auto bound = device_.bind_pipeline_layout(layout);
    if (!bound) {
        return core::Result<rhi::RenderResourceHandle>::failure(bound.error().code,
                                                                bound.error().message);
    }
    pipeline.vertex_shader = program->vertex_shader;
    pipeline.fragment_shader = program->fragment_shader;
    pipeline.vertex_entry_point = std::string(program->vertex_entry_point);
    pipeline.fragment_entry_point = std::string(program->fragment_entry_point);
    auto created = device_.create_graphics_pipeline(std::move(pipeline));
    if (!created) {
        return core::Result<rhi::RenderResourceHandle>::failure(created.error().code,
                                                                created.error().message);
    }
    return core::Result<rhi::RenderResourceHandle>::success(created.value().handle);
}

void PipelineCache::record_failure(const GraphicsPipelineKey& key, const core::Error& error) {
    ++stats_.failed_pipeline_count;
    failures_.push_back({key, error.code, error.message});
}

std::uint64_t
hash_vertex_layout(std::uint32_t stride,
                   std::span<const rhi::RenderVertexAttributeDesc> attributes) noexcept {
    core::StableHash64 hash;
    hash.add_u64_le(stride);
    for (const auto& attribute : attributes) {
        hash.add_u64_le(attribute.location);
        hash.add_u64_le(attribute.byte_offset);
        hash.add_u64_le(static_cast<std::uint64_t>(attribute.format));
    }
    return hash.nonzero_value();
}

std::string_view render_phase_name(RenderPhase phase) noexcept {
    switch (phase) {
    case RenderPhase::sky:
        return "sky";
    case RenderPhase::opaque_terrain:
        return "opaque_terrain";
    case RenderPhase::alpha_tested_terrain:
        return "alpha_tested_terrain";
    case RenderPhase::transparent_terrain:
        return "transparent_terrain";
    case RenderPhase::fluid_terrain:
        return "fluid_terrain";
    case RenderPhase::static_instances:
        return "static_instances";
    case RenderPhase::shadow:
        return "shadow";
    case RenderPhase::debug:
        return "debug";
    case RenderPhase::post_process:
        return "post_process";
    case RenderPhase::ui:
        return "ui";
    }
    return "unknown";
}

} // namespace heartstead::renderer
