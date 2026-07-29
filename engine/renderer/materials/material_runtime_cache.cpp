#include "engine/renderer/materials/material_runtime_cache.hpp"

#include <algorithm>
#include <cmath>
#include <exception>
#include <limits>
#include <ranges>
#include <utility>

namespace heartstead::renderer {

struct MaterialRuntimeCache::Record {
    std::uint32_t generation = 1;
    bool occupied = false;
    MaterialRuntimeDesc desc;
    MaterialRuntimeView view;
};

MaterialRuntimeCache::MaterialRuntimeCache(rhi::IRenderDevice& device)
    : device_(device), gpu_table_(device) {}

MaterialRuntimeCache::~MaterialRuntimeCache() {
    (void)shutdown();
}

core::Result<MaterialRuntimeHandle> MaterialRuntimeCache::upsert(MaterialRuntimeDesc desc) {
    auto status = validate_material_runtime_desc(desc);
    if (!status) {
        return core::Result<MaterialRuntimeHandle>::failure(status.error().code,
                                                            status.error().message);
    }
    const auto existing = std::ranges::find_if(records_, [&desc](const Record& record) {
        return record.occupied && record.desc.id == desc.id;
    });
    if (existing != records_.end()) {
        if (existing->desc.domain != desc.domain ||
            existing->desc.voxel_type != desc.voxel_type) {
            return core::Result<MaterialRuntimeHandle>::failure(
                "material_runtime.identity_change_rejected",
                "resident material cannot change its stable domain or material-table index");
        }
        if (existing->desc == desc) {
            return core::Result<MaterialRuntimeHandle>::success(existing->view.handle);
        }
        if (desc.domain == MaterialRuntimeDomain::voxel) {
            status = table_.set(desc.voxel_type, gpu_voxel_material(desc));
            if (!status) {
                return core::Result<MaterialRuntimeHandle>::failure(status.error().code,
                                                                    status.error().message);
            }
        }
        existing->desc = std::move(desc);
        if (existing->view.revision == std::numeric_limits<std::uint64_t>::max()) {
            std::terminate();
        }
        ++existing->view.revision;
        update_stats();
        return core::Result<MaterialRuntimeHandle>::success(existing->view.handle);
    }
    if (desc.domain == MaterialRuntimeDomain::voxel) {
        const auto conflicting = std::ranges::find_if(records_, [&desc](const Record& record) {
            return record.occupied && record.desc.domain == MaterialRuntimeDomain::voxel &&
                   record.desc.voxel_type == desc.voxel_type;
        });
        if (conflicting != records_.end()) {
            return core::Result<MaterialRuntimeHandle>::failure(
                "material_runtime.duplicate_voxel_type",
                "two runtime materials cannot own the same voxel/material-table index");
        }
        status = table_.set(desc.voxel_type, gpu_voxel_material(desc));
        if (!status) {
            return core::Result<MaterialRuntimeHandle>::failure(status.error().code,
                                                                status.error().message);
        }
    }

    std::size_t slot = records_.size();
    for (std::size_t index = 0; index < records_.size(); ++index) {
        if (!records_[index].occupied) {
            slot = index;
            break;
        }
    }
    if (slot == records_.size()) {
        records_.emplace_back();
    }
    auto& record = records_[slot];
    record.occupied = true;
    record.desc = std::move(desc);
    record.view.handle = {static_cast<std::uint32_t>(slot + 1), record.generation};
    record.view.id = record.desc.id;
    record.view.domain = record.desc.domain;
    record.view.voxel_type = record.desc.voxel_type;
    record.view.material_index =
        record.desc.domain == MaterialRuntimeDomain::voxel ? record.desc.voxel_type : 0;
    record.view.revision = 1;
    update_stats();
    return core::Result<MaterialRuntimeHandle>::success(record.view.handle);
}

core::Status MaterialRuntimeCache::synchronize_gpu() {
    auto status = gpu_table_.synchronize(table_);
    update_stats();
    return status;
}

core::Status
MaterialRuntimeCache::write_gpu_table_descriptor(const core::PrototypeId& pipeline_material,
                                                 std::string binding_name) {
    if (!gpu_table_.buffer().is_valid()) {
        return core::Status::failure("material_runtime.gpu_table_not_ready",
                                     "GPU material table must be synchronized before binding");
    }
    const rhi::RenderDescriptorWrite write{pipeline_material, std::move(binding_name),
                                           gpu_table_.buffer(), 0,
                                           gpu_table_.stats().resident_bytes};
    auto written =
        device_.write_descriptors(std::span<const rhi::RenderDescriptorWrite>{&write, 1});
    if (!written) {
        return core::Status::failure(written.error().code, written.error().message);
    }
    return core::Status::ok();
}

core::Status MaterialRuntimeCache::shutdown() {
    auto status = gpu_table_.shutdown();
    records_.clear();
    table_ = BlockRenderTable{};
    update_stats();
    return status;
}

const MaterialRuntimeView* MaterialRuntimeCache::find(MaterialRuntimeHandle handle) const noexcept {
    const auto* record = find_record(handle);
    return record == nullptr ? nullptr : &record->view;
}

const MaterialRuntimeView* MaterialRuntimeCache::find(const core::PrototypeId& id) const noexcept {
    const auto found = std::ranges::find_if(
        records_, [&id](const Record& record) { return record.occupied && record.desc.id == id; });
    return found == records_.end() ? nullptr : &found->view;
}

const BlockRenderTable& MaterialRuntimeCache::block_render_table() const noexcept {
    return table_;
}

rhi::RenderResourceHandle MaterialRuntimeCache::gpu_table_buffer() const noexcept {
    return gpu_table_.buffer();
}

const MaterialRuntimeCacheStats& MaterialRuntimeCache::stats() const noexcept {
    return stats_;
}

MaterialRuntimeCache::Record*
MaterialRuntimeCache::find_record(MaterialRuntimeHandle handle) noexcept {
    if (!handle.is_valid() || handle.index > records_.size()) {
        return nullptr;
    }
    auto& record = records_[handle.index - 1];
    return record.occupied && record.generation == handle.generation ? &record : nullptr;
}

const MaterialRuntimeCache::Record*
MaterialRuntimeCache::find_record(MaterialRuntimeHandle handle) const noexcept {
    if (!handle.is_valid() || handle.index > records_.size()) {
        return nullptr;
    }
    const auto& record = records_[handle.index - 1];
    return record.occupied && record.generation == handle.generation ? &record : nullptr;
}

void MaterialRuntimeCache::update_stats() noexcept {
    stats_.resident_material_count = static_cast<std::size_t>(
        std::ranges::count_if(records_, [](const Record& record) { return record.occupied; }));
    stats_.table_revision = table_.revision();
    stats_.gpu_table = gpu_table_.stats();
}

TerrainTextureArrayBuilder::TerrainTextureArrayBuilder(std::uint32_t layer_width,
                                                       std::uint32_t layer_height)
    : layer_width_(layer_width), layer_height_(layer_height) {}

core::Result<std::uint32_t>
TerrainTextureArrayBuilder::add_layer(std::string id, std::span<const std::byte> rgba8) {
    if (layer_width_ == 0 || layer_height_ == 0) {
        return core::Result<std::uint32_t>::failure("terrain_texture_array.invalid_extent",
                                                    "texture array layer extent must be nonzero");
    }
    if (id.empty()) {
        return core::Result<std::uint32_t>::failure("terrain_texture_array.missing_layer_id",
                                                    "texture array layer id must not be empty");
    }
    const auto expected = static_cast<std::uint64_t>(layer_width_) * layer_height_ * 4U;
    if (expected > std::numeric_limits<std::size_t>::max() || rgba8.size() != expected) {
        return core::Result<std::uint32_t>::failure(
            "terrain_texture_array.layer_size_mismatch",
            "texture array layer RGBA8 bytes must match the configured extent");
    }
    const auto existing =
        std::ranges::find_if(layers_, [&id](const Layer& layer) { return layer.id == id; });
    if (existing != layers_.end()) {
        if (!std::ranges::equal(existing->rgba8, rgba8)) {
            return core::Result<std::uint32_t>::failure(
                "terrain_texture_array.layer_id_conflict",
                "texture array layer id was reused with different pixels");
        }
        return core::Result<std::uint32_t>::success(
            static_cast<std::uint32_t>(std::distance(layers_.begin(), existing)));
    }
    if (layers_.size() >= std::numeric_limits<std::uint32_t>::max()) {
        return core::Result<std::uint32_t>::failure("terrain_texture_array.too_many_layers",
                                                    "texture array layer index overflow");
    }
    layers_.push_back({std::move(id), {rgba8.begin(), rgba8.end()}});
    return core::Result<std::uint32_t>::success(static_cast<std::uint32_t>(layers_.size() - 1U));
}

core::Result<TextureUploadDesc> TerrainTextureArrayBuilder::build(std::string texture_id,
                                                                  TextureColorSpace color_space,
                                                                  bool generate_mipmaps) const {
    if (texture_id.empty() || layers_.empty()) {
        return core::Result<TextureUploadDesc>::failure(
            "terrain_texture_array.incomplete",
            "terrain texture array requires an id and at least one layer");
    }
    TextureUploadDesc result;
    result.id = std::move(texture_id);
    result.width = layer_width_;
    result.height = layer_height_;
    result.array_layers = static_cast<std::uint32_t>(layers_.size());
    result.color_space = color_space;
    result.generate_mipmaps = generate_mipmaps;
    result.rgba8.reserve(static_cast<std::size_t>(layer_width_) * layer_height_ * 4U *
                         layers_.size());
    for (const auto& layer : layers_) {
        result.rgba8.insert(result.rgba8.end(), layer.rgba8.begin(), layer.rgba8.end());
    }
    return core::Result<TextureUploadDesc>::success(std::move(result));
}

std::size_t TerrainTextureArrayBuilder::layer_count() const noexcept {
    return layers_.size();
}

const std::string* TerrainTextureArrayBuilder::layer_id(std::uint32_t layer) const noexcept {
    return layer < layers_.size() ? &layers_[layer].id : nullptr;
}

core::Status validate_material_runtime_desc(const MaterialRuntimeDesc& desc) {
    if (!desc.id.is_valid()) {
        return core::Status::failure(
            "material_runtime.invalid_identity",
            "runtime material requires a valid prototype id");
    }
    if ((desc.domain == MaterialRuntimeDomain::voxel && desc.voxel_type == 0) ||
        (desc.domain == MaterialRuntimeDomain::surface && desc.voxel_type != 0)) {
        return core::Status::failure(
            "material_runtime.invalid_domain",
            "voxel materials require a non-air voxel type and surface materials require type zero");
    }
    for (const auto value : desc.base_color) {
        if (!std::isfinite(value) || value < 0.0F || value > 1.0F) {
            return core::Status::failure("material_runtime.invalid_base_color",
                                         "material base color must be finite and in zero to one");
        }
    }
    if (!std::isfinite(desc.emissive_strength) || desc.emissive_strength < 0.0F ||
        !std::isfinite(desc.roughness) || desc.roughness < 0.0F || desc.roughness > 1.0F ||
        !std::isfinite(desc.animation_frame_time) || desc.animation_frame_time < 0.0F) {
        return core::Status::failure("material_runtime.invalid_parameters",
                                     "material scalar parameters are outside valid ranges");
    }
    return core::Status::ok();
}

GpuVoxelMaterial gpu_voxel_material(const MaterialRuntimeDesc& desc) noexcept {
    GpuVoxelMaterial result;
    result.side_texture = desc.side_texture;
    result.top_texture = desc.top_texture;
    result.bottom_texture = desc.bottom_texture;
    result.flags = static_cast<std::uint32_t>(desc.flags);
    std::ranges::copy(desc.base_color, result.base_color);
    result.emissive_strength = desc.emissive_strength;
    result.roughness = desc.roughness;
    result.animation_frame_time = desc.animation_frame_time;
    return result;
}

} // namespace heartstead::renderer
