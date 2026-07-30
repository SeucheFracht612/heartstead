#include "engine/renderer/materials/material_runtime_cache.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <exception>
#include <limits>
#include <ranges>
#include <utility>

namespace heartstead::renderer {

namespace {

[[nodiscard]] std::size_t next_surface_table_capacity(std::size_t required) noexcept {
    constexpr std::size_t minimum = 4096;
    return std::max(minimum, std::bit_ceil(required));
}

} // namespace

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
        if (existing->desc.domain != desc.domain || existing->desc.voxel_type != desc.voxel_type) {
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
        } else {
            surface_table_[existing->view.material_index] = gpu_surface_material(desc);
            if (surface_table_revision_ == std::numeric_limits<std::uint64_t>::max()) {
                std::terminate();
            }
            ++surface_table_revision_;
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
    record.view.material_index = record.desc.domain == MaterialRuntimeDomain::voxel
                                     ? record.desc.voxel_type
                                     : static_cast<std::uint32_t>(surface_table_.size());
    if (record.desc.domain == MaterialRuntimeDomain::surface) {
        if (surface_table_.size() >= std::numeric_limits<std::uint32_t>::max()) {
            record = {};
            return core::Result<MaterialRuntimeHandle>::failure(
                "material_runtime.too_many_surface_materials",
                "surface material-table index overflow");
        }
        surface_table_.push_back(gpu_surface_material(record.desc));
        if (surface_table_revision_ == std::numeric_limits<std::uint64_t>::max()) {
            std::terminate();
        }
        ++surface_table_revision_;
    }
    record.view.revision = 1;
    update_stats();
    return core::Result<MaterialRuntimeHandle>::success(record.view.handle);
}

core::Status MaterialRuntimeCache::synchronize_gpu() {
    auto status = gpu_table_.synchronize(table_);
    if (!status) {
        update_stats();
        return status;
    }
    if (stats_.surface_gpu_table.resident_revision != surface_table_revision_) {
        const auto bytes = std::as_bytes(std::span<const GpuSurfaceMaterial>{surface_table_});
        if (bytes.empty()) {
            return core::Status::failure(
                "material_runtime.surface_table_empty",
                "surface material table requires at least the renderer fallback material");
        }
        auto destination = surface_gpu_table_buffer_;
        auto destination_capacity = surface_gpu_table_capacity_;
        bool replacement = false;
        if (!destination.is_valid() || bytes.size() > destination_capacity) {
            destination_capacity = next_surface_table_capacity(bytes.size());
            auto created = device_.create_buffer(
                {rhi::RenderBufferUsage::storage, destination_capacity,
                 "gpu_surface_material_table", rhi::RenderBufferMemory::device_local});
            if (!created) {
                return core::Status::failure(created.error().code, created.error().message);
            }
            destination = created.value().handle;
            replacement = true;
        }
        const rhi::RenderBufferWrite write{destination, 0, bytes};
        auto uploaded =
            device_.upload_buffer_batch(std::span<const rhi::RenderBufferWrite>{&write, 1});
        if (!uploaded) {
            if (replacement) {
                (void)device_.release_resource(destination);
            }
            return core::Status::failure(uploaded.error().code, uploaded.error().message);
        }
        if (replacement) {
            const auto old = surface_gpu_table_buffer_;
            surface_gpu_table_buffer_ = destination;
            surface_gpu_table_capacity_ = destination_capacity;
            if (old.is_valid()) {
                status = device_.release_resource(old);
                if (!status) {
                    return status;
                }
            }
        }
        stats_.surface_gpu_table.material_count = surface_table_.size();
        stats_.surface_gpu_table.resident_bytes = bytes.size();
        stats_.surface_gpu_table.capacity_bytes = surface_gpu_table_capacity_;
        stats_.surface_gpu_table.resident_revision = surface_table_revision_;
        ++stats_.surface_gpu_table.upload_count;
        stats_.surface_gpu_table.uploaded_bytes += bytes.size();
    }
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

core::Status
MaterialRuntimeCache::write_gpu_surface_table_descriptor(const core::PrototypeId& pipeline_material,
                                                         std::string binding_name) {
    if (!surface_gpu_table_buffer_.is_valid()) {
        return core::Status::failure(
            "material_runtime.surface_gpu_table_not_ready",
            "GPU surface material table must be synchronized before binding");
    }
    const rhi::RenderDescriptorWrite write{pipeline_material, std::move(binding_name),
                                           surface_gpu_table_buffer_, 0,
                                           stats_.surface_gpu_table.resident_bytes};
    auto written =
        device_.write_descriptors(std::span<const rhi::RenderDescriptorWrite>{&write, 1});
    if (!written) {
        return core::Status::failure(written.error().code, written.error().message);
    }
    return core::Status::ok();
}

core::Status MaterialRuntimeCache::shutdown() {
    auto status = gpu_table_.shutdown();
    if (surface_gpu_table_buffer_.is_valid()) {
        auto surface_status = device_.release_resource(surface_gpu_table_buffer_);
        if (!surface_status && status) {
            status = surface_status;
        }
    }
    surface_gpu_table_buffer_ = {};
    surface_gpu_table_capacity_ = 0;
    surface_table_.clear();
    surface_table_revision_ = 1;
    stats_.surface_gpu_table = {};
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

std::optional<MaterialRuntimeDesc>
MaterialRuntimeCache::describe(MaterialRuntimeHandle handle) const noexcept {
    const auto* record = find_record(handle);
    return record == nullptr ? std::nullopt : std::optional<MaterialRuntimeDesc>{record->desc};
}

const BlockRenderTable& MaterialRuntimeCache::block_render_table() const noexcept {
    return table_;
}

rhi::RenderResourceHandle MaterialRuntimeCache::gpu_table_buffer() const noexcept {
    return gpu_table_.buffer();
}

rhi::RenderResourceHandle MaterialRuntimeCache::surface_gpu_table_buffer() const noexcept {
    return surface_gpu_table_buffer_;
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
        return core::Status::failure("material_runtime.invalid_identity",
                                     "runtime material requires a valid prototype id");
    }
    if ((desc.domain == MaterialRuntimeDomain::voxel && desc.voxel_type == 0) ||
        (desc.domain == MaterialRuntimeDomain::surface && desc.voxel_type != 0)) {
        return core::Status::failure(
            "material_runtime.invalid_domain",
            "voxel materials require a non-air voxel type and surface materials require type zero");
    }
    for (std::size_t index = 0; index < desc.face_texture_starts.size(); ++index) {
        const auto start = desc.face_texture_starts[index];
        const auto count = desc.face_texture_counts[index];
        if (count == 0 || start > std::numeric_limits<std::uint32_t>::max() - (count - 1U)) {
            return core::Status::failure(
                "material_runtime.invalid_face_texture_range",
                "voxel material face texture ranges must be non-empty and addressable");
        }
    }
    if (!std::isfinite(desc.alpha_cutoff) || desc.alpha_cutoff < 0.0F) {
        return core::Status::failure("material_runtime.invalid_alpha_cutoff",
                                     "material alpha cutoff must be finite and non-negative");
    }
    for (const auto value : desc.base_color) {
        if (!std::isfinite(value) || value < 0.0F || value > 1.0F) {
            return core::Status::failure("material_runtime.invalid_base_color",
                                         "material base color must be finite and in zero to one");
        }
    }
    const auto valid_texture_binding = [](const RuntimeSurfaceTextureBinding& binding) {
        return binding.texcoord <= 1U && std::isfinite(binding.offset[0]) &&
               std::isfinite(binding.offset[1]) && std::isfinite(binding.scale[0]) &&
               std::isfinite(binding.scale[1]) && std::isfinite(binding.rotation);
    };
    if (!valid_texture_binding(desc.base_color_texture) ||
        !valid_texture_binding(desc.metallic_roughness_texture) ||
        !valid_texture_binding(desc.normal_texture) ||
        !valid_texture_binding(desc.occlusion_texture) ||
        !valid_texture_binding(desc.emissive_texture) ||
        !std::ranges::all_of(desc.emissive_color,
                             [](float value) { return std::isfinite(value) && value >= 0.0F; }) ||
        !std::isfinite(desc.metallic) || desc.metallic < 0.0F || desc.metallic > 1.0F ||
        !std::isfinite(desc.normal_scale) || !std::isfinite(desc.occlusion_strength) ||
        desc.occlusion_strength < 0.0F || desc.occlusion_strength > 1.0F) {
        return core::Status::failure(
            "material_runtime.invalid_surface_parameters",
            "surface material texture transforms and PBR parameters must be finite and bounded");
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
    std::ranges::copy(desc.face_texture_starts, result.face_texture_starts);
    std::ranges::copy(desc.face_texture_counts, result.face_texture_counts);
    result.flags = static_cast<std::uint32_t>(desc.flags);
    std::ranges::copy(desc.base_color, result.base_color);
    result.emissive_strength = desc.emissive_strength;
    result.roughness = desc.roughness;
    result.animation_frame_time = desc.animation_frame_time;
    return result;
}

GpuSurfaceMaterial gpu_surface_material(const MaterialRuntimeDesc& desc) noexcept {
    GpuSurfaceMaterial result;
    const auto write_binding = [](GpuSurfaceTextureBinding& target,
                                  const RuntimeSurfaceTextureBinding& source) {
        target.metadata[0] = source.texture;
        target.metadata[1] = source.sampler_state;
        target.metadata[2] = source.texcoord;
        target.metadata[3] = std::bit_cast<std::uint32_t>(source.rotation);
        target.transform[0] = source.offset[0];
        target.transform[1] = source.offset[1];
        target.transform[2] = source.scale[0];
        target.transform[3] = source.scale[1];
    };
    auto base_binding = desc.base_color_texture;
    if (base_binding.texture == 0U) {
        base_binding.texture = desc.surface_texture;
    }
    write_binding(result.textures[0], base_binding);
    write_binding(result.textures[1], desc.metallic_roughness_texture);
    write_binding(result.textures[2], desc.normal_texture);
    write_binding(result.textures[3], desc.occlusion_texture);
    write_binding(result.textures[4], desc.emissive_texture);
    std::ranges::copy(desc.base_color, result.base_color);
    std::ranges::copy(desc.emissive_color, result.emissive_metallic);
    result.emissive_metallic[3] = desc.metallic;
    result.roughness_normal_occlusion_alpha[0] = desc.roughness;
    result.roughness_normal_occlusion_alpha[1] = desc.normal_scale;
    result.roughness_normal_occlusion_alpha[2] = desc.occlusion_strength;
    result.roughness_normal_occlusion_alpha[3] = desc.alpha_cutoff;
    result.flags = static_cast<std::uint32_t>(desc.flags);
    return result;
}

} // namespace heartstead::renderer
