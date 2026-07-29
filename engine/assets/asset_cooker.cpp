#include "engine/assets/asset_cooker.hpp"

#include "engine/assets/image_asset.hpp"
#include "engine/assets/model_asset.hpp"
#include "engine/audio/procedural_tone.hpp"
#include "engine/core/file_io.hpp"
#include "engine/core/hash.hpp"
#include "engine/renderer/shaders/shader_compiler.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <fstream>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace heartstead::assets {

namespace {

using CookedAssetMetadataFields = std::vector<std::pair<std::string, std::string>>;

[[nodiscard]] std::string lower_ascii(std::string value) {
    std::ranges::transform(value, value.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return value;
}

void add_metadata(CookedAssetMetadataFields& metadata, std::string key, std::string value) {
    metadata.emplace_back(std::move(key), std::move(value));
}

void add_metadata(CookedAssetMetadataFields& metadata, std::string key, std::uint64_t value) {
    add_metadata(metadata, std::move(key), std::to_string(value));
}

[[nodiscard]] core::Result<std::vector<std::uint8_t>>
read_file_bytes(const std::filesystem::path& path, std::size_t maximum_bytes) {
    auto bytes = core::read_binary_file(path, {.maximum_bytes = maximum_bytes});
    if (!bytes) {
        return core::Result<std::vector<std::uint8_t>>::failure(
            bytes.error().code == "core.file_too_large" ? "asset_cooker.source_too_large"
                                                        : "asset_cooker.read_failed",
            bytes.error().message);
    }
    return bytes;
}

[[nodiscard]] const AssetRecord* find_source_record(const AssetCatalog& catalog,
                                                    const CookedAssetRecord& cooked) noexcept {
    for (const auto* record : catalog.records_for(cooked.logical_id)) {
        if (record->source_kind == cooked.source_kind && record->source_id == cooked.source_id &&
            record->content_hash == cooked.source_hash &&
            record->virtual_path.to_string() == cooked.source_virtual_path.to_string()) {
            return record;
        }
    }
    return nullptr;
}

[[nodiscard]] bool production_pipeline_available(AssetKind kind) noexcept {
    switch (kind) {
    case AssetKind::localization:
    case AssetKind::ui:
    case AssetKind::texture:
    case AssetKind::material:
    case AssetKind::sound:
    case AssetKind::music:
    case AssetKind::font:
    case AssetKind::data:
    case AssetKind::shader:
    case AssetKind::model:
    case AssetKind::unknown:
        return true;
    }
    return false;
}

[[nodiscard]] bool production_pipeline_converts_source(AssetKind kind) noexcept {
    switch (kind) {
    case AssetKind::texture:
    case AssetKind::model:
    case AssetKind::shader:
    case AssetKind::sound:
    case AssetKind::music:
    case AssetKind::font:
        return true;
    case AssetKind::material:
    case AssetKind::localization:
    case AssetKind::ui:
    case AssetKind::data:
    case AssetKind::unknown:
        return false;
    }
    return false;
}

[[nodiscard]] std::string pipeline_unavailable_message(AssetKind kind) {
    return "production asset pipeline is not available for " + std::string(asset_kind_name(kind)) +
           " assets: " +
           std::string(asset_cook_pipeline_name(kind, AssetCookBackend::production_converters));
}

[[nodiscard]] bool bytes_equal(std::span<const std::uint8_t> bytes, std::size_t offset,
                               std::string_view expected) noexcept {
    if (offset + expected.size() > bytes.size()) {
        return false;
    }
    for (std::size_t index = 0; index < expected.size(); ++index) {
        if (bytes[offset + index] != static_cast<std::uint8_t>(expected[index])) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] std::uint16_t read_le_u16(std::span<const std::uint8_t> bytes,
                                        std::size_t offset) noexcept {
    return static_cast<std::uint16_t>(bytes[offset]) |
           static_cast<std::uint16_t>(static_cast<std::uint16_t>(bytes[offset + 1]) << 8U);
}

[[nodiscard]] std::uint32_t read_le_u32(std::span<const std::uint8_t> bytes,
                                        std::size_t offset) noexcept {
    return static_cast<std::uint32_t>(bytes[offset]) |
           (static_cast<std::uint32_t>(bytes[offset + 1]) << 8U) |
           (static_cast<std::uint32_t>(bytes[offset + 2]) << 16U) |
           (static_cast<std::uint32_t>(bytes[offset + 3]) << 24U);
}

[[nodiscard]] std::uint64_t read_le_u64(std::span<const std::uint8_t> bytes,
                                        std::size_t offset) noexcept {
    return static_cast<std::uint64_t>(read_le_u32(bytes, offset)) |
           (static_cast<std::uint64_t>(read_le_u32(bytes, offset + 4)) << 32U);
}

[[nodiscard]] std::uint16_t read_be_u16(std::span<const std::uint8_t> bytes,
                                        std::size_t offset) noexcept {
    return static_cast<std::uint16_t>(static_cast<std::uint16_t>(bytes[offset]) << 8U) |
           static_cast<std::uint16_t>(bytes[offset + 1]);
}

[[nodiscard]] std::uint32_t read_be_u32(std::span<const std::uint8_t> bytes,
                                        std::size_t offset) noexcept {
    return (static_cast<std::uint32_t>(bytes[offset]) << 24U) |
           (static_cast<std::uint32_t>(bytes[offset + 1]) << 16U) |
           (static_cast<std::uint32_t>(bytes[offset + 2]) << 8U) |
           static_cast<std::uint32_t>(bytes[offset + 3]);
}

[[nodiscard]] core::Status validate_png_texture(std::span<const std::uint8_t> bytes,
                                                const AssetRecord& source) {
    constexpr std::array<std::uint8_t, 8> png_signature{
        0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A,
    };
    if (bytes.size() < 33 ||
        !std::equal(png_signature.begin(), png_signature.end(), bytes.begin())) {
        return core::Status::failure("asset_cooker.invalid_texture",
                                     "production texture asset is not a PNG file: " +
                                         source.logical_id);
    }
    if (read_be_u32(bytes, 8) != 13 || !bytes_equal(bytes, 12, "IHDR")) {
        return core::Status::failure("asset_cooker.invalid_texture",
                                     "production PNG texture is missing a valid IHDR chunk: " +
                                         source.logical_id);
    }

    const auto width = read_be_u32(bytes, 16);
    const auto height = read_be_u32(bytes, 20);
    const auto bit_depth = bytes[24];
    const auto color_type = bytes[25];
    const auto compression = bytes[26];
    const auto filter = bytes[27];
    const auto interlace = bytes[28];
    const auto supported_color_type =
        color_type == 0 || color_type == 2 || color_type == 3 || color_type == 4 || color_type == 6;
    if (width == 0 || height == 0 || bit_depth == 0 || !supported_color_type || compression != 0 ||
        filter != 0 || interlace > 1) {
        return core::Status::failure("asset_cooker.invalid_texture",
                                     "production PNG texture IHDR fields are invalid: " +
                                         source.logical_id);
    }
    return core::Status::ok();
}

[[nodiscard]] core::Status validate_ktx2_texture(std::span<const std::uint8_t> bytes,
                                                 const AssetRecord& source) {
    constexpr std::array<std::uint8_t, 12> ktx2_identifier{
        0xAB, 'K', 'T', 'X', ' ', '2', '0', 0xBB, 0x0D, 0x0A, 0x1A, 0x0A,
    };
    if (bytes.size() < 80 ||
        !std::equal(ktx2_identifier.begin(), ktx2_identifier.end(), bytes.begin())) {
        return core::Status::failure("asset_cooker.invalid_texture",
                                     "production texture asset is not a KTX2 file: " +
                                         source.logical_id);
    }

    const auto type_size = read_le_u32(bytes, 16);
    const auto pixel_width = read_le_u32(bytes, 20);
    const auto pixel_height = read_le_u32(bytes, 24);
    const auto face_count = read_le_u32(bytes, 36);
    const auto level_count = read_le_u32(bytes, 40);
    const auto dfd_offset = static_cast<std::size_t>(read_le_u32(bytes, 48));
    const auto dfd_length = static_cast<std::size_t>(read_le_u32(bytes, 52));
    if (type_size == 0 || pixel_width == 0 || pixel_height == 0 ||
        (face_count != 1 && face_count != 6) || level_count == 0 || dfd_length == 0 ||
        dfd_offset > bytes.size() || dfd_length > bytes.size() - dfd_offset) {
        return core::Status::failure("asset_cooker.invalid_texture",
                                     "production KTX2 texture header fields are invalid: " +
                                         source.logical_id);
    }

    constexpr std::size_t ktx2_header_bytes = 80;
    constexpr std::size_t ktx2_level_index_bytes = 24;
    if (level_count > (bytes.size() - ktx2_header_bytes) / ktx2_level_index_bytes) {
        return core::Status::failure("asset_cooker.invalid_texture",
                                     "production KTX2 texture level index is truncated: " +
                                         source.logical_id);
    }
    bool found_level_payload = false;
    for (std::uint32_t level = 0; level < level_count; ++level) {
        const auto entry_offset =
            ktx2_header_bytes + static_cast<std::size_t>(level) * ktx2_level_index_bytes;
        const auto byte_offset = read_le_u64(bytes, entry_offset);
        const auto byte_length = read_le_u64(bytes, entry_offset + 8);
        if (byte_offset > bytes.size() || byte_length > bytes.size() - byte_offset) {
            return core::Status::failure("asset_cooker.invalid_texture",
                                         "production KTX2 texture level points outside the file: " +
                                             source.logical_id);
        }
        found_level_payload = found_level_payload || byte_length > 0;
    }
    if (!found_level_payload) {
        return core::Status::failure("asset_cooker.invalid_texture",
                                     "production KTX2 texture has no level payload: " +
                                         source.logical_id);
    }

    return core::Status::ok();
}

[[nodiscard]] bool is_jpeg_start_of_frame_marker(std::uint8_t marker) noexcept {
    switch (marker) {
    case 0xC0:
    case 0xC1:
    case 0xC2:
    case 0xC3:
    case 0xC5:
    case 0xC6:
    case 0xC7:
    case 0xC9:
    case 0xCA:
    case 0xCB:
    case 0xCD:
    case 0xCE:
    case 0xCF:
        return true;
    default:
        return false;
    }
}

[[nodiscard]] bool is_jpeg_standalone_marker(std::uint8_t marker) noexcept {
    return marker == 0x01 || (marker >= 0xD0 && marker <= 0xD9);
}

[[nodiscard]] core::Status validate_jpeg_texture(std::span<const std::uint8_t> bytes,
                                                 const AssetRecord& source) {
    if (bytes.size() < 4 || bytes[0] != 0xFF || bytes[1] != 0xD8) {
        return core::Status::failure("asset_cooker.invalid_texture",
                                     "production texture asset is not a JPEG file: " +
                                         source.logical_id);
    }

    bool found_frame = false;
    bool found_end = false;
    std::size_t cursor = 2;
    while (cursor < bytes.size()) {
        if (bytes[cursor] != 0xFF) {
            return core::Status::failure("asset_cooker.invalid_texture",
                                         "production JPEG texture marker stream is invalid: " +
                                             source.logical_id);
        }
        while (cursor < bytes.size() && bytes[cursor] == 0xFF) {
            ++cursor;
        }
        if (cursor >= bytes.size()) {
            break;
        }

        const auto marker = bytes[cursor++];
        if (marker == 0x00) {
            continue;
        }
        if (marker == 0xD9) {
            found_end = true;
            break;
        }
        if (is_jpeg_standalone_marker(marker)) {
            continue;
        }
        if (cursor + 2 > bytes.size()) {
            return core::Status::failure("asset_cooker.invalid_texture",
                                         "production JPEG texture segment length is truncated: " +
                                             source.logical_id);
        }

        const auto segment_length = static_cast<std::size_t>(read_be_u16(bytes, cursor));
        if (segment_length < 2 || segment_length - 2 > bytes.size() - cursor - 2) {
            return core::Status::failure("asset_cooker.invalid_texture",
                                         "production JPEG texture segment range is invalid: " +
                                             source.logical_id);
        }

        const auto segment_payload = cursor + 2;
        if (is_jpeg_start_of_frame_marker(marker)) {
            if (segment_length < 8) {
                return core::Status::failure(
                    "asset_cooker.invalid_texture",
                    "production JPEG texture frame segment is too small: " + source.logical_id);
            }
            const auto precision = bytes[segment_payload];
            const auto height = read_be_u16(bytes, segment_payload + 1);
            const auto width = read_be_u16(bytes, segment_payload + 3);
            const auto component_count = bytes[segment_payload + 5];
            if (precision == 0 || width == 0 || height == 0 || component_count == 0 ||
                8U + static_cast<std::size_t>(component_count) * 3U > segment_length) {
                return core::Status::failure("asset_cooker.invalid_texture",
                                             "production JPEG texture frame fields are invalid: " +
                                                 source.logical_id);
            }
            found_frame = true;
        }

        cursor += segment_length;
        if (marker == 0xDA) {
            while (cursor + 1 < bytes.size()) {
                if (bytes[cursor] != 0xFF) {
                    ++cursor;
                    continue;
                }
                const auto entropy_marker = bytes[cursor + 1];
                if (entropy_marker == 0x00 || entropy_marker == 0xFF ||
                    (entropy_marker >= 0xD0 && entropy_marker <= 0xD7)) {
                    cursor += 2;
                    continue;
                }
                if (entropy_marker == 0xD9) {
                    found_end = true;
                }
                break;
            }
            break;
        }
    }

    if (!found_frame || !found_end) {
        return core::Status::failure(
            "asset_cooker.invalid_texture",
            "production JPEG texture must contain a frame and EOI marker: " + source.logical_id);
    }
    return core::Status::ok();
}

[[nodiscard]] core::Status validate_texture_payload(std::span<const std::uint8_t> bytes,
                                                    const AssetRecord& source) {
    const auto extension = lower_ascii(source.source_path.extension().generic_string());
    if (extension == ".png") {
        return validate_png_texture(bytes, source);
    }
    if (extension == ".ktx2") {
        return validate_ktx2_texture(bytes, source);
    }
    if (extension == ".jpg" || extension == ".jpeg") {
        return validate_jpeg_texture(bytes, source);
    }
    return core::Status::failure("asset_cooker.invalid_texture",
                                 "production texture assets must be PNG, KTX2, or JPEG: " +
                                     source.logical_id);
}

[[nodiscard]] std::string trim_ascii_whitespace(std::string_view value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return {};
    }
    const auto last = value.find_last_not_of(" \t\r\n");
    return std::string(value.substr(first, last - first + 1U));
}

[[nodiscard]] core::Status validate_gltf_json_text(std::string text, const AssetRecord& source) {
    text = trim_ascii_whitespace(text);
    if (text.size() < 2 || text.front() != '{' || text.back() != '}') {
        return core::Status::failure("asset_cooker.invalid_model",
                                     "production glTF model must be a JSON object: " +
                                         source.logical_id);
    }
    if (text.find("\"asset\"") == std::string::npos ||
        text.find("\"version\"") == std::string::npos ||
        text.find("\"2.0\"") == std::string::npos) {
        return core::Status::failure("asset_cooker.invalid_model",
                                     "production glTF model must declare asset version 2.0: " +
                                         source.logical_id);
    }
    return core::Status::ok();
}

[[nodiscard]] std::string bytes_to_string(std::span<const std::uint8_t> bytes) {
    if (bytes.empty()) {
        return {};
    }
    return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

[[nodiscard]] core::Status validate_text_gltf_model(std::span<const std::uint8_t> bytes,
                                                    const AssetRecord& source) {
    if (bytes.empty()) {
        return core::Status::failure("asset_cooker.invalid_model",
                                     "production glTF model is empty: " + source.logical_id);
    }
    return validate_gltf_json_text(bytes_to_string(bytes), source);
}

[[nodiscard]] core::Status validate_glb_model(std::span<const std::uint8_t> bytes,
                                              const AssetRecord& source) {
    constexpr std::uint32_t glb_magic = 0x46546C67U;
    constexpr std::uint32_t glb_json_chunk_type = 0x4E4F534AU;
    constexpr std::size_t glb_header_bytes = 12;
    constexpr std::size_t glb_chunk_header_bytes = 8;

    if (bytes.size() < glb_header_bytes + glb_chunk_header_bytes) {
        return core::Status::failure("asset_cooker.invalid_model",
                                     "production GLB model is too small: " + source.logical_id);
    }
    if (read_le_u32(bytes, 0) != glb_magic) {
        return core::Status::failure("asset_cooker.invalid_model",
                                     "production GLB model has an invalid magic word: " +
                                         source.logical_id);
    }
    if (read_le_u32(bytes, 4) != 2) {
        return core::Status::failure("asset_cooker.invalid_model",
                                     "production GLB model must use glTF 2.0: " +
                                         source.logical_id);
    }
    const auto declared_length = static_cast<std::size_t>(read_le_u32(bytes, 8));
    if (declared_length != bytes.size()) {
        return core::Status::failure("asset_cooker.invalid_model",
                                     "production GLB model declared length does not match file: " +
                                         source.logical_id);
    }

    auto cursor = glb_header_bytes;
    const auto first_chunk_length = static_cast<std::size_t>(read_le_u32(bytes, cursor));
    const auto first_chunk_type = read_le_u32(bytes, cursor + 4);
    if (first_chunk_type != glb_json_chunk_type || first_chunk_length == 0 ||
        first_chunk_length % 4U != 0) {
        return core::Status::failure("asset_cooker.invalid_model",
                                     "production GLB model must start with a JSON chunk: " +
                                         source.logical_id);
    }
    cursor += glb_chunk_header_bytes;
    if (cursor > bytes.size() || first_chunk_length > bytes.size() - cursor) {
        return core::Status::failure("asset_cooker.invalid_model",
                                     "production GLB JSON chunk points outside the file: " +
                                         source.logical_id);
    }
    auto status =
        validate_gltf_json_text(bytes_to_string(bytes.subspan(cursor, first_chunk_length)), source);
    if (!status) {
        return status;
    }
    cursor += first_chunk_length;

    while (cursor < bytes.size()) {
        if (bytes.size() - cursor < glb_chunk_header_bytes) {
            return core::Status::failure("asset_cooker.invalid_model",
                                         "production GLB model has a truncated chunk header: " +
                                             source.logical_id);
        }
        const auto chunk_length = static_cast<std::size_t>(read_le_u32(bytes, cursor));
        cursor += glb_chunk_header_bytes;
        if (chunk_length % 4U != 0 || cursor > bytes.size() ||
            chunk_length > bytes.size() - cursor) {
            return core::Status::failure("asset_cooker.invalid_model",
                                         "production GLB model chunk range is invalid: " +
                                             source.logical_id);
        }
        cursor += chunk_length;
    }

    return core::Status::ok();
}

[[nodiscard]] core::Status validate_model_payload(std::span<const std::uint8_t> bytes,
                                                  const AssetRecord& source) {
    const auto extension = lower_ascii(source.source_path.extension().generic_string());
    if (extension == ".gltf") {
        return validate_text_gltf_model(bytes, source);
    }
    if (extension == ".glb") {
        return validate_glb_model(bytes, source);
    }
    return core::Status::failure("asset_cooker.invalid_model",
                                 "production model assets must be glTF or GLB: " +
                                     source.logical_id);
}

[[nodiscard]] core::Status validate_shader_payload(std::span<const std::uint8_t> bytes,
                                                   const AssetRecord& source) {
    using renderer::shaders::ShaderSourceLanguage;

    const auto language =
        renderer::shaders::infer_shader_language(source.virtual_path.relative_path);
    switch (language) {
    case ShaderSourceLanguage::spirv:
        return renderer::shaders::validate_spirv_shader_bytes(bytes,
                                                              source.virtual_path.to_string());
    case ShaderSourceLanguage::slang:
    case ShaderSourceLanguage::hlsl:
        return core::Status::failure(
            "shader_compiler.production_compiler_unavailable",
            "production shader compilation is not available for " +
                std::string(renderer::shaders::shader_source_language_name(language)) +
                " source yet: " + source.virtual_path.to_string());
    case ShaderSourceLanguage::unknown:
        return core::Status::failure(
            "asset_cooker.invalid_shader",
            "production shader asset has an unsupported source language: " +
                source.virtual_path.to_string());
    }
    return core::Status::failure("asset_cooker.invalid_shader",
                                 "production shader asset has an unsupported source language: " +
                                     source.virtual_path.to_string());
}

[[nodiscard]] core::Status validate_wav_audio(std::span<const std::uint8_t> bytes,
                                              const AssetRecord& source) {
    if (bytes.size() < 12 || !bytes_equal(bytes, 0, "RIFF") || !bytes_equal(bytes, 8, "WAVE")) {
        return core::Status::failure("asset_cooker.invalid_wav",
                                     "production audio asset is not a RIFF/WAVE file: " +
                                         source.logical_id);
    }

    const auto declared_size = static_cast<std::size_t>(read_le_u32(bytes, 4)) + 8U;
    if (declared_size < 12 || declared_size > bytes.size()) {
        return core::Status::failure("asset_cooker.invalid_wav",
                                     "production audio RIFF size is invalid: " + source.logical_id);
    }

    bool found_format = false;
    bool found_data = false;
    std::size_t cursor = 12;
    while (cursor + 8 <= declared_size) {
        const auto chunk_size = static_cast<std::size_t>(read_le_u32(bytes, cursor + 4));
        const auto chunk_data_start = cursor + 8;
        const auto chunk_data_end = chunk_data_start + chunk_size;
        if (chunk_data_end < chunk_data_start || chunk_data_end > declared_size) {
            return core::Status::failure("asset_cooker.invalid_wav",
                                         "production audio chunk size is invalid: " +
                                             source.logical_id);
        }

        if (bytes_equal(bytes, cursor, "fmt ")) {
            if (chunk_size < 16) {
                return core::Status::failure("asset_cooker.invalid_wav",
                                             "production audio fmt chunk is too small: " +
                                                 source.logical_id);
            }
            const auto audio_format = read_le_u16(bytes, chunk_data_start);
            const auto channel_count = read_le_u16(bytes, chunk_data_start + 2);
            const auto sample_rate = read_le_u32(bytes, chunk_data_start + 4);
            const auto bits_per_sample = read_le_u16(bytes, chunk_data_start + 14);
            if ((audio_format != 1 && audio_format != 3) || channel_count == 0 ||
                sample_rate == 0 || bits_per_sample == 0) {
                return core::Status::failure("asset_cooker.invalid_wav",
                                             "production audio fmt chunk is invalid: " +
                                                 source.logical_id);
            }
            found_format = true;
        } else if (bytes_equal(bytes, cursor, "data")) {
            if (chunk_size == 0) {
                return core::Status::failure("asset_cooker.invalid_wav",
                                             "production audio data chunk is empty: " +
                                                 source.logical_id);
            }
            found_data = true;
        }

        cursor = chunk_data_end + (chunk_size % 2U);
    }

    if (!found_format || !found_data) {
        return core::Status::failure("asset_cooker.invalid_wav",
                                     "production audio asset must contain fmt and data chunks: " +
                                         source.logical_id);
    }
    return core::Status::ok();
}

[[nodiscard]] core::Status validate_ogg_audio(std::span<const std::uint8_t> bytes,
                                              const AssetRecord& source) {
    constexpr std::size_t ogg_header_bytes = 27;
    if (bytes.size() < ogg_header_bytes || !bytes_equal(bytes, 0, "OggS")) {
        return core::Status::failure("asset_cooker.invalid_ogg",
                                     "production OGG audio asset is missing an OggS page: " +
                                         source.logical_id);
    }
    if (bytes[4] != 0) {
        return core::Status::failure("asset_cooker.invalid_ogg",
                                     "production OGG audio stream version is unsupported: " +
                                         source.logical_id);
    }

    const auto segment_count = static_cast<std::size_t>(bytes[26]);
    if (segment_count == 0 || segment_count > bytes.size() - ogg_header_bytes) {
        return core::Status::failure("asset_cooker.invalid_ogg",
                                     "production OGG audio page segment table is invalid: " +
                                         source.logical_id);
    }

    std::size_t payload_bytes = 0;
    for (std::size_t index = 0; index < segment_count; ++index) {
        payload_bytes += bytes[ogg_header_bytes + index];
    }
    const auto payload_offset = ogg_header_bytes + segment_count;
    if (payload_bytes == 0 || payload_offset > bytes.size() ||
        payload_bytes > bytes.size() - payload_offset) {
        return core::Status::failure("asset_cooker.invalid_ogg",
                                     "production OGG audio page payload is invalid: " +
                                         source.logical_id);
    }
    return core::Status::ok();
}

[[nodiscard]] std::uint32_t read_be_u24(std::span<const std::uint8_t> bytes,
                                        std::size_t offset) noexcept {
    return (static_cast<std::uint32_t>(bytes[offset]) << 16U) |
           (static_cast<std::uint32_t>(bytes[offset + 1]) << 8U) |
           static_cast<std::uint32_t>(bytes[offset + 2]);
}

[[nodiscard]] core::Status validate_flac_audio(std::span<const std::uint8_t> bytes,
                                               const AssetRecord& source) {
    constexpr std::size_t flac_magic_bytes = 4;
    constexpr std::size_t flac_metadata_header_bytes = 4;
    constexpr std::size_t flac_streaminfo_bytes = 34;
    if (bytes.size() < flac_magic_bytes + flac_metadata_header_bytes + flac_streaminfo_bytes ||
        !bytes_equal(bytes, 0, "fLaC")) {
        return core::Status::failure("asset_cooker.invalid_flac",
                                     "production FLAC audio asset is missing a FLAC stream: " +
                                         source.logical_id);
    }

    const auto first_block_type = bytes[4] & 0x7FU;
    const auto first_block_length = static_cast<std::size_t>(read_be_u24(bytes, 5));
    if (first_block_type != 0 || first_block_length != flac_streaminfo_bytes ||
        first_block_length > bytes.size() - flac_magic_bytes - flac_metadata_header_bytes) {
        return core::Status::failure("asset_cooker.invalid_flac",
                                     "production FLAC audio must start with a STREAMINFO block: " +
                                         source.logical_id);
    }
    return core::Status::ok();
}

[[nodiscard]] core::Status validate_audio_payload(std::span<const std::uint8_t> bytes,
                                                  const AssetRecord& source) {
    const auto extension = lower_ascii(source.source_path.extension().generic_string());
    if (extension == ".wav") {
        return validate_wav_audio(bytes, source);
    }
    if (extension == ".ogg") {
        return validate_ogg_audio(bytes, source);
    }
    if (extension == ".flac") {
        return validate_flac_audio(bytes, source);
    }
    if (extension == ".tone") {
        auto tone = audio::load_procedural_tone_asset(source.source_path, 48'000);
        if (!tone) {
            return core::Status::failure(tone.error().code, tone.error().message);
        }
        return core::Status::ok();
    }
    return core::Status::failure("asset_cooker.invalid_audio",
                                 "production audio assets must be WAV, OGG, FLAC, or TONE: " +
                                     source.logical_id);
}

[[nodiscard]] core::Status validate_sfnt_font(std::span<const std::uint8_t> bytes,
                                              const AssetRecord& source) {
    if (bytes.size() < 12) {
        return core::Status::failure("asset_cooker.invalid_font",
                                     "production font asset is too small: " + source.logical_id);
    }

    if (bytes_equal(bytes, 0, "ttcf")) {
        const auto font_count = read_be_u32(bytes, 8);
        if (font_count == 0 || bytes.size() < 12U + static_cast<std::size_t>(font_count) * 4U) {
            return core::Status::failure("asset_cooker.invalid_font",
                                         "production TrueType collection header is invalid: " +
                                             source.logical_id);
        }
        for (std::uint32_t index = 0; index < font_count; ++index) {
            const auto offset = static_cast<std::size_t>(read_be_u32(bytes, 12U + index * 4U));
            if (offset > bytes.size() || bytes.size() - offset < 12U) {
                return core::Status::failure(
                    "asset_cooker.invalid_font",
                    "production TrueType collection font offset is invalid: " + source.logical_id);
            }
        }
        return core::Status::ok();
    }

    const auto sfnt_version = read_be_u32(bytes, 0);
    const auto supported_version = sfnt_version == 0x00010000U || bytes_equal(bytes, 0, "OTTO") ||
                                   bytes_equal(bytes, 0, "true");
    if (!supported_version) {
        return core::Status::failure("asset_cooker.invalid_font",
                                     "production font asset is not a supported SFNT font: " +
                                         source.logical_id);
    }

    const auto table_count = read_be_u16(bytes, 4);
    if (table_count == 0) {
        return core::Status::failure("asset_cooker.invalid_font",
                                     "production font asset has no SFNT tables: " +
                                         source.logical_id);
    }

    constexpr std::size_t sfnt_header_bytes = 12;
    constexpr std::size_t table_record_bytes = 16;
    const auto table_directory_bytes = static_cast<std::size_t>(table_count) * table_record_bytes;
    if (table_directory_bytes > bytes.size() ||
        sfnt_header_bytes > bytes.size() - table_directory_bytes) {
        return core::Status::failure("asset_cooker.invalid_font",
                                     "production font table directory is truncated: " +
                                         source.logical_id);
    }

    for (std::uint16_t index = 0; index < table_count; ++index) {
        const auto table_record_offset =
            sfnt_header_bytes + static_cast<std::size_t>(index) * table_record_bytes;
        const auto table_offset =
            static_cast<std::size_t>(read_be_u32(bytes, table_record_offset + 8));
        const auto table_length =
            static_cast<std::size_t>(read_be_u32(bytes, table_record_offset + 12));
        if (table_offset > bytes.size() || table_length > bytes.size() - table_offset) {
            return core::Status::failure("asset_cooker.invalid_font",
                                         "production font table points outside the file: " +
                                             source.logical_id);
        }
    }

    return core::Status::ok();
}

[[nodiscard]] core::Status
validate_production_source_payload(const AssetRecord& source,
                                   std::span<const std::uint8_t> source_bytes) {
    switch (source.kind) {
    case AssetKind::texture:
        return validate_texture_payload(source_bytes, source);
    case AssetKind::shader:
        return validate_shader_payload(source_bytes, source);
    case AssetKind::model:
        return validate_model_payload(source_bytes, source);
    case AssetKind::sound:
    case AssetKind::music:
        return validate_audio_payload(source_bytes, source);
    case AssetKind::font:
        return validate_sfnt_font(source_bytes, source);
    case AssetKind::material:
    case AssetKind::localization:
    case AssetKind::ui:
    case AssetKind::data:
    case AssetKind::unknown:
        return core::Status::ok();
    }
    return core::Status::ok();
}

[[nodiscard]] CookedAssetMetadataFields texture_metadata(std::span<const std::uint8_t> bytes,
                                                         const AssetRecord& source) {
    CookedAssetMetadataFields metadata;
    const auto extension = lower_ascii(source.source_path.extension().generic_string());

    if (extension == ".png" && bytes.size() >= 29) {
        add_metadata(metadata, "texture.container", "png");
        add_metadata(metadata, "texture.width", read_be_u32(bytes, 16));
        add_metadata(metadata, "texture.height", read_be_u32(bytes, 20));
        add_metadata(metadata, "texture.bit_depth", bytes[24]);
        add_metadata(metadata, "texture.color_type", bytes[25]);
        add_metadata(metadata, "texture.interlace", bytes[28]);
        return metadata;
    }

    if (extension == ".ktx2" && bytes.size() >= 80) {
        add_metadata(metadata, "texture.container", "ktx2");
        add_metadata(metadata, "texture.vk_format", read_le_u32(bytes, 12));
        add_metadata(metadata, "texture.width", read_le_u32(bytes, 20));
        add_metadata(metadata, "texture.height", read_le_u32(bytes, 24));
        add_metadata(metadata, "texture.depth", read_le_u32(bytes, 28));
        add_metadata(metadata, "texture.face_count", read_le_u32(bytes, 36));
        add_metadata(metadata, "texture.level_count", read_le_u32(bytes, 40));
        return metadata;
    }

    if ((extension == ".jpg" || extension == ".jpeg") && bytes.size() >= 4) {
        std::size_t cursor = 2;
        while (cursor < bytes.size()) {
            if (bytes[cursor] != 0xFF) {
                break;
            }
            while (cursor < bytes.size() && bytes[cursor] == 0xFF) {
                ++cursor;
            }
            if (cursor >= bytes.size()) {
                break;
            }

            const auto marker = bytes[cursor++];
            if (marker == 0xD9 || is_jpeg_standalone_marker(marker)) {
                continue;
            }
            if (cursor + 2 > bytes.size()) {
                break;
            }

            const auto segment_length = static_cast<std::size_t>(read_be_u16(bytes, cursor));
            if (segment_length < 2 || segment_length - 2 > bytes.size() - cursor - 2) {
                break;
            }

            const auto segment_payload = cursor + 2;
            if (is_jpeg_start_of_frame_marker(marker) && segment_length >= 8) {
                add_metadata(metadata, "texture.container", "jpeg");
                add_metadata(metadata, "texture.width", read_be_u16(bytes, segment_payload + 3));
                add_metadata(metadata, "texture.height", read_be_u16(bytes, segment_payload + 1));
                add_metadata(metadata, "texture.bit_depth", bytes[segment_payload]);
                add_metadata(metadata, "texture.component_count", bytes[segment_payload + 5]);
                return metadata;
            }
            cursor += segment_length;
        }
    }

    return metadata;
}

[[nodiscard]] CookedAssetMetadataFields model_metadata(std::span<const std::uint8_t> bytes,
                                                       const AssetRecord& source) {
    CookedAssetMetadataFields metadata;
    const auto extension = lower_ascii(source.source_path.extension().generic_string());
    if (extension == ".gltf") {
        add_metadata(metadata, "model.container", "gltf");
        add_metadata(metadata, "model.gltf_version", "2.0");
        return metadata;
    }

    if (extension == ".glb" && bytes.size() >= 20) {
        constexpr std::size_t glb_header_bytes = 12;
        constexpr std::size_t glb_chunk_header_bytes = 8;
        std::size_t cursor = glb_header_bytes;
        std::uint32_t chunk_count = 0;
        std::uint32_t json_chunk_bytes = 0;
        while (cursor + glb_chunk_header_bytes <= bytes.size()) {
            const auto chunk_length = static_cast<std::size_t>(read_le_u32(bytes, cursor));
            const auto chunk_type = read_le_u32(bytes, cursor + 4);
            cursor += glb_chunk_header_bytes;
            if (chunk_length > bytes.size() - cursor) {
                break;
            }
            ++chunk_count;
            if (chunk_type == 0x4E4F534AU) {
                json_chunk_bytes = static_cast<std::uint32_t>(chunk_length);
            }
            cursor += chunk_length;
        }

        add_metadata(metadata, "model.container", "glb");
        add_metadata(metadata, "model.gltf_version", read_le_u32(bytes, 4));
        add_metadata(metadata, "model.chunk_count", chunk_count);
        add_metadata(metadata, "model.json_chunk_bytes", json_chunk_bytes);
    }
    return metadata;
}

[[nodiscard]] CookedAssetMetadataFields shader_metadata(std::span<const std::uint8_t> bytes,
                                                        const AssetRecord& source) {
    CookedAssetMetadataFields metadata;
    const auto extension = lower_ascii(source.source_path.extension().generic_string());
    if (extension == ".spv" && bytes.size() >= 20 && bytes.size() % sizeof(std::uint32_t) == 0) {
        add_metadata(metadata, "shader.container", "spirv");
        add_metadata(metadata, "shader.word_count", bytes.size() / sizeof(std::uint32_t));
        add_metadata(metadata, "shader.version_word", read_le_u32(bytes, 4));
        add_metadata(metadata, "shader.bound", read_le_u32(bytes, 12));
    }
    return metadata;
}

[[nodiscard]] CookedAssetMetadataFields audio_metadata(std::span<const std::uint8_t> bytes,
                                                       const AssetRecord& source) {
    CookedAssetMetadataFields metadata;
    const auto extension = lower_ascii(source.source_path.extension().generic_string());

    if (extension == ".wav" && bytes.size() >= 12) {
        add_metadata(metadata, "audio.container", "wav");
        add_metadata(metadata, "audio.riff_bytes",
                     static_cast<std::uint64_t>(read_le_u32(bytes, 4)) + 8U);
        std::size_t cursor = 12;
        while (cursor + 8 <= bytes.size()) {
            const auto chunk_size = static_cast<std::size_t>(read_le_u32(bytes, cursor + 4));
            const auto chunk_data_start = cursor + 8;
            if (chunk_size > bytes.size() - chunk_data_start) {
                break;
            }
            if (bytes_equal(bytes, cursor, "fmt ") && chunk_size >= 16) {
                add_metadata(metadata, "audio.format_code", read_le_u16(bytes, chunk_data_start));
                add_metadata(metadata, "audio.channels", read_le_u16(bytes, chunk_data_start + 2));
                add_metadata(metadata, "audio.sample_rate",
                             read_le_u32(bytes, chunk_data_start + 4));
                add_metadata(metadata, "audio.bits_per_sample",
                             read_le_u16(bytes, chunk_data_start + 14));
            } else if (bytes_equal(bytes, cursor, "data")) {
                add_metadata(metadata, "audio.data_bytes", chunk_size);
            }
            cursor = chunk_data_start + chunk_size + (chunk_size % 2U);
        }
        return metadata;
    }

    if (extension == ".ogg" && bytes.size() >= 27) {
        const auto segment_count = static_cast<std::size_t>(bytes[26]);
        std::uint64_t first_page_payload_bytes = 0;
        for (std::size_t index = 0; index < segment_count && 27U + index < bytes.size(); ++index) {
            first_page_payload_bytes += bytes[27U + index];
        }
        add_metadata(metadata, "audio.container", "ogg");
        add_metadata(metadata, "audio.first_page_segments", segment_count);
        add_metadata(metadata, "audio.first_page_payload_bytes", first_page_payload_bytes);
        return metadata;
    }

    if (extension == ".flac" && bytes.size() >= 8) {
        add_metadata(metadata, "audio.container", "flac");
        add_metadata(metadata, "audio.first_block_type", bytes[4] & 0x7FU);
        add_metadata(metadata, "audio.first_block_bytes", read_be_u24(bytes, 5));
        return metadata;
    }

    if (extension == ".tone") {
        auto tone = audio::load_procedural_tone_asset(source.source_path, 48'000);
        if (tone) {
            add_metadata(metadata, "audio.container", "tone");
            add_metadata(metadata, "audio.channels", 1);
            add_metadata(metadata, "audio.sample_rate", tone.value().sample_rate);
            add_metadata(metadata, "audio.frames", tone.value().mono_samples.size());
        }
        return metadata;
    }

    return metadata;
}

[[nodiscard]] CookedAssetMetadataFields font_metadata(std::span<const std::uint8_t> bytes,
                                                      const AssetRecord&) {
    CookedAssetMetadataFields metadata;
    if (bytes.size() < 12) {
        return metadata;
    }
    if (bytes_equal(bytes, 0, "ttcf")) {
        add_metadata(metadata, "font.container", "ttc");
        add_metadata(metadata, "font.face_count", read_be_u32(bytes, 8));
        return metadata;
    }
    add_metadata(metadata, "font.container", "sfnt");
    add_metadata(metadata, "font.table_count", read_be_u16(bytes, 4));
    if (bytes_equal(bytes, 0, "OTTO")) {
        add_metadata(metadata, "font.outline", "cff");
    } else {
        add_metadata(metadata, "font.outline", "truetype");
    }
    return metadata;
}

[[nodiscard]] CookedAssetMetadataFields
production_metadata_fields(const AssetRecord& source, std::span<const std::uint8_t> source_bytes) {
    switch (source.kind) {
    case AssetKind::texture:
        return texture_metadata(source_bytes, source);
    case AssetKind::model:
        return model_metadata(source_bytes, source);
    case AssetKind::shader:
        return shader_metadata(source_bytes, source);
    case AssetKind::sound:
    case AssetKind::music:
        return audio_metadata(source_bytes, source);
    case AssetKind::font:
        return font_metadata(source_bytes, source);
    case AssetKind::material:
    case AssetKind::localization:
    case AssetKind::ui:
    case AssetKind::data:
    case AssetKind::unknown:
        return {};
    }
    return {};
}

void add_model_runtime_metadata(CookedAssetMetadataFields& metadata, const ModelAsset& model) {
    add_metadata(metadata, "model.runtime_format", "heartstead.model.v2");
    add_metadata(metadata, "model.vertices", model.vertices.size());
    add_metadata(metadata, "model.indices", model.indices.size());
    add_metadata(metadata, "model.nodes", model.nodes.size());
    add_metadata(metadata, "model.primitives", model.primitives.size());
    add_metadata(metadata, "model.images", model.images.size());
    add_metadata(metadata, "model.materials", model.materials.size());
    add_metadata(metadata, "model.skins", model.skins.size());
    add_metadata(metadata, "model.animations", model.animations.size());
}

void add_texture_runtime_metadata(CookedAssetMetadataFields& metadata, const ImageAsset& image,
                                  std::string_view source_container) {
    metadata.erase(std::remove_if(metadata.begin(), metadata.end(),
                                  [](const auto& field) {
                                      return field.first == "texture.container" ||
                                             field.first == "texture.width" ||
                                             field.first == "texture.height";
                                  }),
                   metadata.end());
    add_metadata(metadata, "texture.source_container", std::string(source_container));
    add_metadata(metadata, "texture.container", "rgba8");
    add_metadata(metadata, "texture.runtime_format", "heartstead.texture.rgba8.v1");
    add_metadata(metadata, "texture.width", image.width);
    add_metadata(metadata, "texture.height", image.height);
    add_metadata(metadata, "texture.channels", 4);
    add_metadata(metadata, "texture.color_space", "unspecified");
}

[[nodiscard]] std::vector<std::uint8_t>
build_cooked_payload_bytes(const CookedAssetRecord& cooked, const AssetRecord& source,
                           AssetCookBackend backend, std::string_view profile,
                           std::span<const std::uint8_t> source_bytes,
                           const CookedAssetMetadataFields& metadata) {
    std::ostringstream header;
    header << "heartstead.cooked_asset_payload.v1\n";
    header << "backend=" << asset_cook_pipeline_name(source.kind, backend) << '\n';
    header << "logical_id=" << cooked.logical_id << '\n';
    header << "kind=" << asset_kind_name(cooked.kind) << '\n';
    header << "profile=" << profile << '\n';
    header << "source_virtual_path=" << cooked.source_virtual_path.to_string() << '\n';
    header << "source_hash=" << cooked.source_hash << '\n';
    header << "pipeline_version=" << cooked.pipeline_version << '\n';
    header << "source_bytes=" << source_bytes.size() << '\n';
    for (const auto& [key, value] : metadata) {
        header << "meta." << key << '=' << value << '\n';
    }
    header << "---\n";

    const auto header_text = header.str();
    std::vector<std::uint8_t> payload;
    payload.reserve(header_text.size() + source_bytes.size());
    payload.insert(payload.end(), header_text.begin(), header_text.end());
    payload.insert(payload.end(), source_bytes.begin(), source_bytes.end());
    return payload;
}

[[nodiscard]] core::Status write_cooked_payload(const std::filesystem::path& path,
                                                std::span<const std::uint8_t> payload) {
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    if (error) {
        return core::Status::failure("asset_cooker.create_directory_failed", error.message());
    }

    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        return core::Status::failure("asset_cooker.write_failed",
                                     "failed to open cooked asset output: " + path.string());
    }

    if (!payload.empty()) {
        output.write(reinterpret_cast<const char*>(payload.data()),
                     static_cast<std::streamsize>(payload.size()));
    }
    if (!output) {
        return core::Status::failure("asset_cooker.write_failed",
                                     "failed to write cooked asset output: " + path.string());
    }

    return core::Status::ok();
}

[[nodiscard]] core::Status write_manifest(const std::filesystem::path& path,
                                          const CookedAssetManifest& manifest) {
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    if (error) {
        return core::Status::failure("asset_cooker.create_directory_failed", error.message());
    }

    std::ofstream output(path, std::ios::trunc);
    if (!output) {
        return core::Status::failure("asset_cooker.write_failed",
                                     "failed to open cooked asset manifest: " + path.string());
    }

    output << CookedAssetManifestTextCodec::encode(manifest);
    if (!output) {
        return core::Status::failure("asset_cooker.write_failed",
                                     "failed to write cooked asset manifest: " + path.string());
    }
    return core::Status::ok();
}

} // namespace

core::Result<AssetCookResult> AssetCooker::cook(const AssetCatalog& catalog,
                                                AssetCookConfig config) {
    auto config_status = validate_asset_cook_config(config);
    if (!config_status) {
        return core::Result<AssetCookResult>::failure(config_status.error().code,
                                                      config_status.error().message);
    }
    if (config.backend == AssetCookBackend::production_converters) {
        config.manifest_config.profile = "production";
    }

    const auto backend_info = asset_cook_backend_info(config.backend);
    if (!backend_info.available) {
        return core::Result<AssetCookResult>::failure("asset_cooker.backend_unavailable",
                                                      "asset cook backend is not available: " +
                                                          std::string(backend_info.name));
    }

    for (const auto* source : catalog.records()) {
        std::error_code size_error;
        const auto size = std::filesystem::file_size(source->source_path, size_error);
        if (size_error) {
            return core::Result<AssetCookResult>::failure("asset_cooker.read_failed",
                                                          size_error.message());
        }
        if (size > config.maximum_source_bytes) {
            return core::Result<AssetCookResult>::failure(
                "asset_cooker.source_too_large",
                "asset source exceeds the configured byte limit: " + source->logical_id);
        }
    }

    auto resolved_catalog = catalog;
    if (config.backend == AssetCookBackend::production_converters) {
        auto dependency_status = discover_asset_dependencies(resolved_catalog);
        if (!dependency_status) {
            return core::Result<AssetCookResult>::failure(
                dependency_status.error().code.starts_with("gltf_import.")
                    ? "asset_cooker.invalid_model"
                    : dependency_status.error().code,
                dependency_status.error().message);
        }
    }
    auto manifest = CookedAssetManifestBuilder::build(resolved_catalog, config.manifest_config);
    if (!manifest) {
        return core::Result<AssetCookResult>::failure(manifest.error().code,
                                                      manifest.error().message);
    }

    auto output_root = canonical_asset_root(config.output_root);
    if (!output_root) {
        return core::Result<AssetCookResult>::failure(output_root.error().code,
                                                      output_root.error().message);
    }
    auto manifest_path = resolve_asset_path(output_root.value(), config.manifest_relative_path);
    if (!manifest_path) {
        return core::Result<AssetCookResult>::failure(manifest_path.error().code,
                                                      manifest_path.error().message);
    }

    AssetCookResult result;
    result.backend = config.backend;
    result.manifest = std::move(manifest).value();
    result.manifest_path = std::move(manifest_path).value();

    for (auto& cooked : result.manifest.records) {
        const auto* source = find_source_record(resolved_catalog, cooked);
        if (source == nullptr) {
            return core::Result<AssetCookResult>::failure(
                "asset_cooker.missing_source_record",
                "cooked asset has no matching source record: " + cooked.logical_id);
        }
        if (source->source_path.empty()) {
            return core::Result<AssetCookResult>::failure(
                "asset_cooker.missing_source_path",
                "asset source record has no physical source path: " + source->logical_id);
        }
        const auto pipeline_info = asset_cook_pipeline_info(source->kind, config.backend);
        if (!pipeline_info.available) {
            return core::Result<AssetCookResult>::failure(
                "asset_cooker.pipeline_unavailable", pipeline_unavailable_message(source->kind));
        }

        auto source_bytes = read_file_bytes(source->source_path, config.maximum_source_bytes);
        if (!source_bytes) {
            return core::Result<AssetCookResult>::failure(source_bytes.error().code,
                                                          source_bytes.error().message);
        }
        std::vector<std::uint8_t> converted_bytes;
        std::optional<ModelAsset> imported_model;
        std::optional<ImageAsset> imported_texture;
        const std::vector<std::uint8_t>* runtime_bytes = &source_bytes.value();
        if (config.backend == AssetCookBackend::production_converters &&
            source->kind == AssetKind::model) {
            ModelAssetLimits model_limits;
            model_limits.maximum_source_bytes = config.maximum_source_bytes;
            auto imported = import_gltf_model(source->source_path, model_limits);
            if (!imported) {
                return core::Result<AssetCookResult>::failure(
                    "asset_cooker.invalid_model",
                    "production model import failed for " + source->logical_id + ": " +
                        imported.error().code + ": " + imported.error().message);
            }
            auto encoded = encode_model_asset(imported.value(), model_limits);
            if (!encoded) {
                return core::Result<AssetCookResult>::failure(
                    "asset_cooker.invalid_model",
                    "production model encoding failed for " + source->logical_id + ": " +
                        encoded.error().code + ": " + encoded.error().message);
            }
            imported_model = std::move(imported).value();
            converted_bytes = std::move(encoded).value();
            runtime_bytes = &converted_bytes;
        } else if (config.backend == AssetCookBackend::production_converters &&
                   source->kind == AssetKind::texture) {
            const auto extension = lower_ascii(source->source_path.extension().generic_string());
            if (extension == ".png" || extension == ".jpg" || extension == ".jpeg") {
                ImageAssetLimits image_limits;
                image_limits.maximum_decoded_bytes = config.maximum_source_bytes;
                auto decoded = decode_png_or_jpeg(source_bytes.value(), image_limits);
                if (!decoded) {
                    return core::Result<AssetCookResult>::failure(
                        "asset_cooker.invalid_texture",
                        "production texture decode failed for " + source->logical_id + ": " +
                            decoded.error().code + ": " + decoded.error().message);
                }
                imported_texture = std::move(decoded).value();
                converted_bytes = std::move(imported_texture->rgba8);
                runtime_bytes = &converted_bytes;
            } else {
                auto status = validate_production_source_payload(*source, source_bytes.value());
                if (!status) {
                    return core::Result<AssetCookResult>::failure(status.error().code,
                                                                  status.error().message);
                }
            }
        } else if (config.backend == AssetCookBackend::production_converters) {
            auto status = validate_production_source_payload(*source, source_bytes.value());
            if (!status) {
                return core::Result<AssetCookResult>::failure(status.error().code,
                                                              status.error().message);
            }
        }
        auto metadata = config.backend == AssetCookBackend::production_converters
                            ? production_metadata_fields(*source, source_bytes.value())
                            : CookedAssetMetadataFields{};
        if (imported_model.has_value()) {
            add_model_runtime_metadata(metadata, *imported_model);
        }
        if (imported_texture.has_value()) {
            const auto extension = lower_ascii(source->source_path.extension().generic_string());
            add_texture_runtime_metadata(metadata, *imported_texture,
                                         extension == ".png" ? "png" : "jpeg");
        }

        const auto payload = build_cooked_payload_bytes(
            cooked, *source, config.backend, result.manifest.profile, *runtime_bytes, metadata);
        cooked.cooked_hash = core::stable_hash64_hex(payload);
        auto output_path = resolve_asset_path(output_root.value(), cooked.cooked_relative_path);
        if (!output_path) {
            return core::Result<AssetCookResult>::failure(output_path.error().code,
                                                          output_path.error().message);
        }
        auto status = write_cooked_payload(output_path.value(), payload);
        if (!status) {
            return core::Result<AssetCookResult>::failure(status.error().code,
                                                          status.error().message);
        }
        ++result.cooked_file_count;
        result.cooked_payload_bytes += runtime_bytes->size();
    }

    auto status = write_manifest(result.manifest_path, result.manifest);
    if (!status) {
        return core::Result<AssetCookResult>::failure(status.error().code, status.error().message);
    }

    return core::Result<AssetCookResult>::success(std::move(result));
}

core::Status validate_asset_cook_config(const AssetCookConfig& config) {
    if (config.output_root.empty()) {
        return core::Status::failure("asset_cooker.invalid_output_root",
                                     "asset cooker output root is required");
    }
    if (!is_safe_asset_relative_path(config.manifest_relative_path)) {
        return core::Status::failure("asset_cooker.invalid_manifest_path",
                                     "asset cooker manifest path must be a safe relative path");
    }
    if (config.maximum_source_bytes == 0) {
        return core::Status::failure("asset_cooker.invalid_source_limit",
                                     "asset cooker source byte limit must be non-zero");
    }

    switch (config.backend) {
    case AssetCookBackend::development_passthrough:
    case AssetCookBackend::production_converters:
        return core::Status::ok();
    }
    return core::Status::failure("asset_cooker.unknown_backend", "unknown asset cook backend");
}

AssetCookBackendInfo asset_cook_backend_info(AssetCookBackend backend) noexcept {
    switch (backend) {
    case AssetCookBackend::development_passthrough:
        return AssetCookBackendInfo{
            AssetCookBackend::development_passthrough,
            asset_cook_backend_name(AssetCookBackend::development_passthrough),
            true,
            "development passthrough cooker available",
        };
    case AssetCookBackend::production_converters:
        return AssetCookBackendInfo{
            AssetCookBackend::production_converters,
            asset_cook_backend_name(AssetCookBackend::production_converters),
            true,
            "partial production converters available for data-like, material, glTF/GLB model, "
            "PNG/KTX2/JPEG texture, SPIR-V shader, WAV/OGG/FLAC/TONE audio, and SFNT font assets",
        };
    }
    return AssetCookBackendInfo{backend, "unknown", false, "unknown asset cook backend"};
}

AssetCookPipelineInfo asset_cook_pipeline_info(AssetKind kind, AssetCookBackend backend) noexcept {
    const auto backend_info = asset_cook_backend_info(backend);
    const auto available = backend == AssetCookBackend::production_converters
                               ? production_pipeline_available(kind)
                               : backend_info.available;
    const auto converts = backend == AssetCookBackend::production_converters
                              ? production_pipeline_converts_source(kind)
                              : false;
    const auto status = backend == AssetCookBackend::production_converters && !available
                            ? "production converter for this asset kind is not linked yet"
                            : backend_info.status;
    return AssetCookPipelineInfo{
        kind, backend, asset_cook_pipeline_name(kind, backend), available, converts, status,
    };
}

std::string_view asset_cook_backend_name(AssetCookBackend backend) noexcept {
    switch (backend) {
    case AssetCookBackend::development_passthrough:
        return "development_passthrough";
    case AssetCookBackend::production_converters:
        return "production_converters";
    }
    return "unknown";
}

std::string_view asset_cook_backend_name(AssetKind kind) noexcept {
    return asset_cook_pipeline_name(kind, AssetCookBackend::development_passthrough);
}

std::string_view asset_cook_pipeline_name(AssetKind kind, AssetCookBackend backend) noexcept {
    if (backend == AssetCookBackend::production_converters) {
        switch (kind) {
        case AssetKind::texture:
            return "texture_png_ktx2_jpeg_converter_v2";
        case AssetKind::model:
            return "model_gltf_runtime_converter_v3";
        case AssetKind::shader:
            return "shader_spirv_runtime_passthrough_v1";
        case AssetKind::sound:
        case AssetKind::music:
            return "audio_runtime_converter_v1";
        case AssetKind::material:
            return "material_runtime_converter_v1";
        case AssetKind::font:
            return "font_runtime_converter_v1";
        case AssetKind::localization:
            return "localization_runtime_converter_v1";
        case AssetKind::ui:
            return "ui_runtime_converter_v1";
        case AssetKind::data:
        case AssetKind::unknown:
            return "data_runtime_converter_v1";
        }
        return "data_runtime_converter_v1";
    }

    switch (kind) {
    case AssetKind::texture:
        return "texture_dev_passthrough_v1";
    case AssetKind::model:
        return "model_dev_passthrough_v1";
    case AssetKind::shader:
        return "shader_dev_passthrough_v1";
    case AssetKind::sound:
    case AssetKind::music:
        return "audio_dev_passthrough_v1";
    case AssetKind::material:
        return "material_dev_passthrough_v1";
    case AssetKind::font:
        return "font_dev_passthrough_v1";
    case AssetKind::localization:
        return "localization_dev_passthrough_v1";
    case AssetKind::ui:
        return "ui_dev_passthrough_v1";
    case AssetKind::data:
    case AssetKind::unknown:
        return "data_dev_passthrough_v1";
    }
    return "data_dev_passthrough_v1";
}

} // namespace heartstead::assets
