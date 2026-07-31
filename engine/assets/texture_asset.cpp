#include "engine/assets/texture_asset.hpp"

#include "engine/modding/flat_manifest.hpp"

#include <ktx.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <map>
#include <memory>
#include <ranges>
#include <string>
#include <utility>

namespace heartstead::assets {

namespace {

constexpr std::string_view texture_magic = "heartstead.texture.v2";

[[nodiscard]] std::string lowercase_ascii(std::string value) {
    std::ranges::transform(value, value.begin(), [](char character) {
        if (character >= 'A' && character <= 'Z') {
            return static_cast<char>(character - 'A' + 'a');
        }
        return character;
    });
    return value;
}

[[nodiscard]] bool contains_token(std::string_view value, std::string_view token) noexcept {
    return value.find(token) != std::string_view::npos;
}

[[nodiscard]] float srgb_to_linear(float value) noexcept {
    return value <= 0.04045F ? value / 12.92F : std::pow((value + 0.055F) / 1.055F, 2.4F);
}

[[nodiscard]] float linear_to_srgb(float value) noexcept {
    return value <= 0.0031308F ? value * 12.92F : 1.055F * std::pow(value, 1.0F / 2.4F) - 0.055F;
}

[[nodiscard]] std::uint8_t encode_unit(float value) noexcept {
    return static_cast<std::uint8_t>(std::lround(std::clamp(value, 0.0F, 1.0F) * 255.0F));
}

[[nodiscard]] std::vector<std::uint8_t> downsample_rgba8(std::span<const std::uint8_t> source,
                                                         std::uint32_t source_width,
                                                         std::uint32_t source_height,
                                                         const TextureCookSettings& settings) {
    const auto width = std::max(1U, source_width / 2U);
    const auto height = std::max(1U, source_height / 2U);
    std::vector<std::uint8_t> result(static_cast<std::size_t>(width) * height * 4U);
    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x < width; ++x) {
            std::array<std::array<float, 4>, 4> samples{};
            std::size_t sample_count = 0;
            for (std::uint32_t dy = 0; dy < 2; ++dy) {
                for (std::uint32_t dx = 0; dx < 2; ++dx) {
                    const auto source_x = std::min(source_width - 1U, x * 2U + dx);
                    const auto source_y = std::min(source_height - 1U, y * 2U + dy);
                    const auto offset =
                        (static_cast<std::size_t>(source_y) * source_width + source_x) * 4U;
                    for (std::size_t channel = 0; channel < 4; ++channel) {
                        samples[sample_count][channel] =
                            static_cast<float>(source[offset + channel]) / 255.0F;
                    }
                    ++sample_count;
                }
            }
            const auto destination = (static_cast<std::size_t>(y) * width + x) * 4U;
            if (settings.role == TextureRole::normal) {
                std::array<float, 3> normal{};
                float alpha = 0.0F;
                for (const auto& sample : samples) {
                    normal[0] += sample[0] * 2.0F - 1.0F;
                    normal[1] += sample[1] * 2.0F - 1.0F;
                    normal[2] += sample[2] * 2.0F - 1.0F;
                    alpha += sample[3];
                }
                const auto length = std::sqrt(normal[0] * normal[0] + normal[1] * normal[1] +
                                              normal[2] * normal[2]);
                if (length > 1.0e-8F && std::isfinite(length)) {
                    for (auto& component : normal) {
                        component /= length;
                    }
                } else {
                    normal = {0.0F, 0.0F, 1.0F};
                }
                result[destination] = encode_unit(normal[0] * 0.5F + 0.5F);
                result[destination + 1U] = encode_unit(normal[1] * 0.5F + 0.5F);
                result[destination + 2U] = encode_unit(normal[2] * 0.5F + 0.5F);
                result[destination + 3U] = encode_unit(alpha * 0.25F);
                continue;
            }
            for (std::size_t channel = 0; channel < 4; ++channel) {
                float sum = 0.0F;
                for (const auto& sample : samples) {
                    const auto decode_srgb =
                        settings.color_space == TextureAssetColorSpace::srgb && channel < 3U;
                    sum += decode_srgb ? srgb_to_linear(sample[channel]) : sample[channel];
                }
                auto value = sum * 0.25F;
                if (settings.color_space == TextureAssetColorSpace::srgb && channel < 3U) {
                    value = linear_to_srgb(value);
                }
                result[destination + channel] = encode_unit(value);
            }
        }
    }
    return result;
}

[[nodiscard]] float alpha_coverage(std::span<const std::uint8_t> rgba8, float cutoff) noexcept {
    if (rgba8.empty()) {
        return 0.0F;
    }
    const auto threshold = encode_unit(cutoff);
    std::size_t covered = 0;
    for (std::size_t offset = 3; offset < rgba8.size(); offset += 4U) {
        covered += rgba8[offset] >= threshold ? 1U : 0U;
    }
    return static_cast<float>(covered) / static_cast<float>(rgba8.size() / 4U);
}

void preserve_alpha_coverage(std::vector<std::uint8_t>& rgba8, float target, float cutoff) {
    if (rgba8.empty() || target <= 0.0F || target >= 1.0F) {
        return;
    }
    float low = 0.0F;
    float high = 8.0F;
    for (std::uint32_t iteration = 0; iteration < 16U; ++iteration) {
        const auto scale = (low + high) * 0.5F;
        std::size_t covered = 0;
        for (std::size_t offset = 3; offset < rgba8.size(); offset += 4U) {
            const auto alpha = std::min(255.0F, static_cast<float>(rgba8[offset]) * scale);
            covered += alpha / 255.0F >= cutoff ? 1U : 0U;
        }
        const auto coverage = static_cast<float>(covered) / static_cast<float>(rgba8.size() / 4U);
        if (coverage < target) {
            low = scale;
        } else {
            high = scale;
        }
    }
    const auto scale = (low + high) * 0.5F;
    for (std::size_t offset = 3; offset < rgba8.size(); offset += 4U) {
        rgba8[offset] = static_cast<std::uint8_t>(
            std::lround(std::min(255.0F, static_cast<float>(rgba8[offset]) * scale)));
    }
}

struct UncompressedMip {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<std::uint8_t> rgba8;
};

[[nodiscard]] std::vector<UncompressedMip> generate_mips(const ImageAsset& source,
                                                         const TextureCookSettings& settings) {
    std::vector<UncompressedMip> result;
    result.push_back({source.width, source.height, source.rgba8});
    if (!settings.generate_mips) {
        return result;
    }
    const auto target_coverage = settings.preserve_alpha_coverage
                                     ? alpha_coverage(source.rgba8, settings.alpha_cutoff)
                                     : 0.0F;
    while (result.back().width > 1U || result.back().height > 1U) {
        const auto& previous = result.back();
        auto next = downsample_rgba8(previous.rgba8, previous.width, previous.height, settings);
        if (settings.preserve_alpha_coverage) {
            preserve_alpha_coverage(next, target_coverage, settings.alpha_cutoff);
        }
        result.push_back({std::max(1U, previous.width / 2U), std::max(1U, previous.height / 2U),
                          std::move(next)});
    }
    return result;
}

[[nodiscard]] std::size_t block_level_bytes(std::uint32_t width, std::uint32_t height) noexcept {
    return static_cast<std::size_t>((width + 3U) / 4U) *
           static_cast<std::size_t>((height + 3U) / 4U) * 16U;
}

[[nodiscard]] core::Result<std::vector<TextureMipLevel>>
compress_mips(std::span<const UncompressedMip> mips, TextureAssetFormat format,
              TextureAssetColorSpace color_space, bool normal_map) {
    if (format == TextureAssetFormat::rgba8) {
        std::vector<TextureMipLevel> result;
        result.reserve(mips.size());
        for (const auto& mip : mips) {
            result.push_back({mip.width, mip.height, mip.rgba8});
        }
        return core::Result<std::vector<TextureMipLevel>>::success(std::move(result));
    }

    ktxTextureCreateInfo info{};
    // Vulkan format values 37 and 43 are RGBA8 UNORM and RGBA8 SRGB.
    info.vkFormat = color_space == TextureAssetColorSpace::srgb ? 43U : 37U;
    info.baseWidth = mips.front().width;
    info.baseHeight = mips.front().height;
    info.baseDepth = 1;
    info.numDimensions = 2;
    info.numLevels = static_cast<std::uint32_t>(mips.size());
    info.numLayers = 1;
    info.numFaces = 1;
    ktxTexture2* raw_texture = nullptr;
    const auto create_result =
        ktxTexture2_Create(&info, KTX_TEXTURE_CREATE_ALLOC_STORAGE, &raw_texture);
    if (create_result != KTX_SUCCESS || raw_texture == nullptr) {
        return core::Result<std::vector<TextureMipLevel>>::failure(
            "texture_asset.ktx_create_failed",
            "failed to create deterministic KTX2 compression input: " +
                std::string(ktxErrorString(create_result)));
    }
    const auto destroy = [](ktxTexture2* texture) { ktxTexture2_Destroy(texture); };
    std::unique_ptr<ktxTexture2, decltype(destroy)> texture(raw_texture, destroy);
    for (std::uint32_t level = 0; level < mips.size(); ++level) {
        const auto& mip = mips[level];
        const auto set_result = ktxTexture_SetImageFromMemory(
            ktxTexture(texture.get()), level, 0, 0, mip.rgba8.data(), mip.rgba8.size());
        if (set_result != KTX_SUCCESS) {
            return core::Result<std::vector<TextureMipLevel>>::failure(
                "texture_asset.ktx_image_failed",
                "failed to populate KTX2 mip input: " + std::string(ktxErrorString(set_result)));
        }
    }

    ktxBasisParams params{};
    params.structSize = sizeof(params);
    params.uastc = KTX_TRUE;
    params.threadCount = 1;
    params.normalMap = normal_map ? KTX_TRUE : KTX_FALSE;
    params.uastcFlags = KTX_PACK_UASTC_LEVEL_DEFAULT;
    params.uastcRDONoMultithreading = KTX_TRUE;
    const auto compress_result = ktxTexture2_CompressBasisEx(texture.get(), &params);
    if (compress_result != KTX_SUCCESS) {
        return core::Result<std::vector<TextureMipLevel>>::failure(
            "texture_asset.compression_failed",
            "Basis Universal rejected texture compression input: " +
                std::string(ktxErrorString(compress_result)));
    }
    const auto target = format == TextureAssetFormat::bc5_rg ? KTX_TTF_BC5_RG : KTX_TTF_BC7_RGBA;
    const auto transcode_result = ktxTexture2_TranscodeBasis(texture.get(), target, 0);
    if (transcode_result != KTX_SUCCESS) {
        return core::Result<std::vector<TextureMipLevel>>::failure(
            "texture_asset.transcode_failed",
            "Basis Universal could not produce the requested GPU-native format: " +
                std::string(ktxErrorString(transcode_result)));
    }

    auto* base = ktxTexture(texture.get());
    std::vector<TextureMipLevel> result;
    result.reserve(mips.size());
    for (std::uint32_t level = 0; level < mips.size(); ++level) {
        ktx_size_t offset = 0;
        const auto offset_result = ktxTexture_GetImageOffset(base, level, 0, 0, &offset);
        const auto expected = block_level_bytes(mips[level].width, mips[level].height);
        if (offset_result != KTX_SUCCESS || base->pData == nullptr || offset > base->dataSize ||
            expected > base->dataSize - offset) {
            return core::Result<std::vector<TextureMipLevel>>::failure(
                "texture_asset.invalid_compressed_layout",
                "compressed KTX2 mip layout does not contain the expected GPU blocks");
        }
        TextureMipLevel mip;
        mip.width = mips[level].width;
        mip.height = mips[level].height;
        mip.bytes.assign(base->pData + offset, base->pData + offset + expected);
        result.push_back(std::move(mip));
    }
    return core::Result<std::vector<TextureMipLevel>>::success(std::move(result));
}

class ByteWriter {
  public:
    void u8(std::uint8_t value) {
        bytes_.push_back(value);
    }
    void u32(std::uint32_t value) {
        for (std::uint32_t shift = 0; shift < 32U; shift += 8U) {
            u8(static_cast<std::uint8_t>((value >> shift) & 0xFFU));
        }
    }
    void f32(float value) {
        u32(std::bit_cast<std::uint32_t>(value));
    }
    void string(std::string_view value) {
        u32(static_cast<std::uint32_t>(value.size()));
        bytes_.insert(bytes_.end(), value.begin(), value.end());
    }
    void bytes(std::span<const std::uint8_t> value) {
        bytes_.insert(bytes_.end(), value.begin(), value.end());
    }
    [[nodiscard]] std::vector<std::uint8_t> take() {
        return std::move(bytes_);
    }

  private:
    std::vector<std::uint8_t> bytes_;
};

class ByteReader {
  public:
    explicit ByteReader(std::span<const std::uint8_t> bytes) : bytes_(bytes) {}

    [[nodiscard]] core::Result<std::span<const std::uint8_t>> bytes(std::size_t count) {
        if (count > bytes_.size() - offset_) {
            return core::Result<std::span<const std::uint8_t>>::failure(
                "texture_asset.truncated", "texture asset payload is truncated");
        }
        const auto value = bytes_.subspan(offset_, count);
        offset_ += count;
        return core::Result<std::span<const std::uint8_t>>::success(value);
    }
    [[nodiscard]] core::Result<std::uint8_t> u8() {
        auto value = bytes(1);
        if (!value) {
            return core::Result<std::uint8_t>::failure(value.error().code, value.error().message);
        }
        return core::Result<std::uint8_t>::success(value.value()[0]);
    }
    [[nodiscard]] core::Result<std::uint32_t> u32() {
        auto value = bytes(4);
        if (!value) {
            return core::Result<std::uint32_t>::failure(value.error().code, value.error().message);
        }
        return core::Result<std::uint32_t>::success(
            static_cast<std::uint32_t>(value.value()[0]) |
            (static_cast<std::uint32_t>(value.value()[1]) << 8U) |
            (static_cast<std::uint32_t>(value.value()[2]) << 16U) |
            (static_cast<std::uint32_t>(value.value()[3]) << 24U));
    }
    [[nodiscard]] core::Result<float> f32() {
        auto value = u32();
        if (!value) {
            return core::Result<float>::failure(value.error().code, value.error().message);
        }
        return core::Result<float>::success(std::bit_cast<float>(value.value()));
    }
    [[nodiscard]] core::Result<std::string> string(std::size_t maximum) {
        auto count = u32();
        if (!count || count.value() > maximum) {
            return core::Result<std::string>::failure("texture_asset.invalid_string",
                                                      "texture asset string is invalid");
        }
        auto value = bytes(count.value());
        if (!value) {
            return core::Result<std::string>::failure(value.error().code, value.error().message);
        }
        return core::Result<std::string>::success(
            std::string(reinterpret_cast<const char*>(value.value().data()), value.value().size()));
    }
    [[nodiscard]] bool at_end() const noexcept {
        return offset_ == bytes_.size();
    }

  private:
    std::span<const std::uint8_t> bytes_;
    std::size_t offset_ = 0;
};

[[nodiscard]] core::Result<bool> parse_boolean(std::string_view value, std::string_view field) {
    if (value == "true") {
        return core::Result<bool>::success(true);
    }
    if (value == "false") {
        return core::Result<bool>::success(false);
    }
    return core::Result<bool>::failure("texture_asset.invalid_sidecar",
                                       "texture sidecar field must be true or false: " +
                                           std::string(field));
}

[[nodiscard]] core::Result<float> parse_cutoff(std::string_view value) {
    try {
        std::size_t consumed = 0;
        const auto result = std::stof(std::string(value), &consumed);
        if (consumed != value.size() || !std::isfinite(result) || result < 0.0F || result > 1.0F) {
            throw std::invalid_argument("range");
        }
        return core::Result<float>::success(result);
    } catch (const std::exception&) {
        return core::Result<float>::failure(
            "texture_asset.invalid_sidecar",
            "texture sidecar alpha_cutoff must be a number in [0, 1]");
    }
}

} // namespace

std::size_t TextureAsset::gpu_memory_bytes() const noexcept {
    std::size_t result = 0;
    for (const auto& mip : mips) {
        result += mip.bytes.size();
    }
    return result;
}

core::Status TextureAssetLimits::validate() const {
    if (maximum_dimension == 0 || maximum_mip_levels == 0 || maximum_payload_bytes < 16U) {
        return core::Status::failure(
            "texture_asset.invalid_limits",
            "texture asset limits must permit a non-empty two-dimensional image");
    }
    return core::Status::ok();
}

TextureCookSettings infer_texture_cook_settings(const std::filesystem::path& source_path) noexcept {
    TextureCookSettings settings;
    const auto name = lowercase_ascii(source_path.stem().generic_string());
    const auto path = lowercase_ascii(source_path.generic_string());
    if (contains_token(name, "_normal") || contains_token(name, "_norm") || name.ends_with("_n")) {
        settings.role = TextureRole::normal;
        settings.color_space = TextureAssetColorSpace::linear;
        settings.compression = TextureCompressionMode::bc5;
    } else if (contains_token(name, "_metallic") || contains_token(name, "_roughness") ||
               contains_token(name, "_orm") || contains_token(name, "_rma")) {
        settings.role = TextureRole::metallic_roughness;
        settings.color_space = TextureAssetColorSpace::linear;
    } else if (contains_token(name, "_ao") || contains_token(name, "_occlusion")) {
        settings.role = TextureRole::occlusion;
        settings.color_space = TextureAssetColorSpace::linear;
    } else if (contains_token(name, "_emissive") || contains_token(name, "_emit")) {
        settings.role = TextureRole::emissive;
    } else if (contains_token(name, "_mask") || contains_token(name, "_cutout")) {
        settings.role = TextureRole::mask;
        settings.color_space = TextureAssetColorSpace::linear;
        settings.preserve_alpha_coverage = true;
    } else if (contains_token(path, "/ui/")) {
        settings.role = TextureRole::user_interface;
    }
    // Voxel textures are currently resampled into a shared runtime array. Keeping them
    // uncompressed is an explicit compatibility fallback until the array cooker owns packing.
    if (contains_token(path, "textures/voxels/")) {
        settings.compression = TextureCompressionMode::rgba8;
    }
    return settings;
}

std::filesystem::path texture_cook_sidecar_path(const std::filesystem::path& source_path) {
    auto result = source_path;
    result += ".texture.toml";
    return result;
}

core::Result<TextureCookSettings>
load_texture_cook_settings(const std::filesystem::path& source_path) {
    auto settings = infer_texture_cook_settings(source_path);
    const auto sidecar = texture_cook_sidecar_path(source_path);
    if (!std::filesystem::exists(sidecar)) {
        return core::Result<TextureCookSettings>::success(settings);
    }
    std::vector<modding::ModDiagnostic> diagnostics;
    const auto values = modding::parse_flat_manifest(sidecar, diagnostics,
                                                     {.diagnostic_prefix = "texture_asset.sidecar",
                                                      .maximum_bytes = 64U * 1024U,
                                                      .maximum_line_bytes = 4U * 1024U,
                                                      .maximum_fields = 16});
    if (!diagnostics.empty()) {
        return core::Result<TextureCookSettings>::failure(diagnostics.front().code,
                                                          sidecar.generic_string() + ": " +
                                                              diagnostics.front().message);
    }
    constexpr std::array allowed{
        std::string_view{"role"},
        std::string_view{"color_space"},
        std::string_view{"compression"},
        std::string_view{"generate_mips"},
        std::string_view{"preserve_alpha_coverage"},
        std::string_view{"alpha_cutoff"},
    };
    for (const auto& [key, value] : values) {
        (void)value;
        if (std::ranges::find(allowed, key) == allowed.end()) {
            return core::Result<TextureCookSettings>::failure(
                "texture_asset.unknown_sidecar_field",
                sidecar.generic_string() + ": unknown texture cook field: " + key);
        }
    }
    if (const auto found = values.find("role"); found != values.end()) {
        const std::map<std::string, TextureRole> roles{
            {"color", TextureRole::color},
            {"normal", TextureRole::normal},
            {"metallic_roughness", TextureRole::metallic_roughness},
            {"occlusion", TextureRole::occlusion},
            {"emissive", TextureRole::emissive},
            {"mask", TextureRole::mask},
            {"ui", TextureRole::user_interface},
            {"data", TextureRole::data},
        };
        const auto role = roles.find(found->second);
        if (role == roles.end()) {
            return core::Result<TextureCookSettings>::failure(
                "texture_asset.invalid_sidecar",
                sidecar.generic_string() + ": unknown texture role: " + found->second);
        }
        settings.role = role->second;
    }
    if (const auto found = values.find("color_space"); found != values.end()) {
        if (found->second == "linear") {
            settings.color_space = TextureAssetColorSpace::linear;
        } else if (found->second == "srgb") {
            settings.color_space = TextureAssetColorSpace::srgb;
        } else {
            return core::Result<TextureCookSettings>::failure(
                "texture_asset.invalid_sidecar",
                sidecar.generic_string() + ": color_space must be linear or srgb");
        }
    }
    if (const auto found = values.find("compression"); found != values.end()) {
        const std::map<std::string, TextureCompressionMode> modes{
            {"automatic", TextureCompressionMode::automatic},
            {"rgba8", TextureCompressionMode::rgba8},
            {"bc5", TextureCompressionMode::bc5},
            {"bc7", TextureCompressionMode::bc7},
        };
        const auto mode = modes.find(found->second);
        if (mode == modes.end()) {
            return core::Result<TextureCookSettings>::failure(
                "texture_asset.invalid_sidecar",
                sidecar.generic_string() + ": unknown compression mode: " + found->second);
        }
        settings.compression = mode->second;
    }
    if (const auto found = values.find("generate_mips"); found != values.end()) {
        auto value = parse_boolean(found->second, found->first);
        if (!value) {
            return core::Result<TextureCookSettings>::failure(value.error().code,
                                                              value.error().message);
        }
        settings.generate_mips = value.value();
    }
    if (const auto found = values.find("preserve_alpha_coverage"); found != values.end()) {
        auto value = parse_boolean(found->second, found->first);
        if (!value) {
            return core::Result<TextureCookSettings>::failure(value.error().code,
                                                              value.error().message);
        }
        settings.preserve_alpha_coverage = value.value();
    }
    if (const auto found = values.find("alpha_cutoff"); found != values.end()) {
        auto value = parse_cutoff(found->second);
        if (!value) {
            return core::Result<TextureCookSettings>::failure(value.error().code,
                                                              value.error().message);
        }
        settings.alpha_cutoff = value.value();
    }
    if (settings.role == TextureRole::normal &&
        settings.color_space != TextureAssetColorSpace::linear) {
        return core::Result<TextureCookSettings>::failure(
            "texture_asset.invalid_sidecar",
            sidecar.generic_string() + ": normal maps must use linear color space");
    }
    return core::Result<TextureCookSettings>::success(settings);
}

core::Result<TextureAsset> cook_texture_asset(const ImageAsset& source,
                                              const TextureCookSettings& settings,
                                              const TextureAssetLimits& limits) {
    auto limit_status = limits.validate();
    if (!limit_status) {
        return core::Result<TextureAsset>::failure(limit_status.error().code,
                                                   limit_status.error().message);
    }
    const auto expected = static_cast<std::uint64_t>(source.width) * source.height * 4U;
    if (source.width == 0 || source.height == 0 || source.width > limits.maximum_dimension ||
        source.height > limits.maximum_dimension || expected != source.rgba8.size() ||
        !std::isfinite(settings.alpha_cutoff) || settings.alpha_cutoff < 0.0F ||
        settings.alpha_cutoff > 1.0F ||
        (settings.role == TextureRole::normal &&
         settings.color_space != TextureAssetColorSpace::linear)) {
        return core::Result<TextureAsset>::failure(
            "texture_asset.invalid_source",
            "texture source dimensions, bytes, role, or cook settings are invalid");
    }
    auto format = TextureAssetFormat::bc7_rgba;
    switch (settings.compression) {
    case TextureCompressionMode::automatic:
        format = settings.role == TextureRole::normal ? TextureAssetFormat::bc5_rg
                                                      : TextureAssetFormat::bc7_rgba;
        break;
    case TextureCompressionMode::rgba8:
        format = TextureAssetFormat::rgba8;
        break;
    case TextureCompressionMode::bc5:
        format = TextureAssetFormat::bc5_rg;
        break;
    case TextureCompressionMode::bc7:
        format = TextureAssetFormat::bc7_rgba;
        break;
    }
    if (format == TextureAssetFormat::bc5_rg && settings.role != TextureRole::normal) {
        return core::Result<TextureAsset>::failure(
            "texture_asset.invalid_compression",
            "BC5 is reserved for two-channel tangent-space normal maps");
    }
    auto uncompressed = generate_mips(source, settings);
    if (uncompressed.size() > limits.maximum_mip_levels) {
        return core::Result<TextureAsset>::failure(
            "texture_asset.mip_limit", "generated texture mip count exceeds its configured limit");
    }
    auto compressed = compress_mips(uncompressed, format, settings.color_space,
                                    settings.role == TextureRole::normal);
    if (!compressed) {
        return core::Result<TextureAsset>::failure(compressed.error().code,
                                                   compressed.error().message);
    }
    TextureAsset asset;
    asset.width = source.width;
    asset.height = source.height;
    asset.role = settings.role;
    asset.color_space = settings.color_space;
    asset.format = format;
    asset.alpha_coverage_preserved = settings.preserve_alpha_coverage;
    asset.alpha_cutoff = settings.alpha_cutoff;
    asset.mips = std::move(compressed).value();
    auto status = validate_texture_asset(asset, limits);
    if (!status) {
        return core::Result<TextureAsset>::failure(status.error().code, status.error().message);
    }
    return core::Result<TextureAsset>::success(std::move(asset));
}

core::Status validate_texture_asset(const TextureAsset& asset, const TextureAssetLimits& limits) {
    auto status = limits.validate();
    if (!status) {
        return status;
    }
    if (asset.width == 0 || asset.height == 0 || asset.width > limits.maximum_dimension ||
        asset.height > limits.maximum_dimension || asset.mips.empty() ||
        asset.mips.size() > limits.maximum_mip_levels || !std::isfinite(asset.alpha_cutoff) ||
        asset.alpha_cutoff < 0.0F || asset.alpha_cutoff > 1.0F ||
        (asset.format == TextureAssetFormat::bc5_rg && asset.role != TextureRole::normal) ||
        (asset.role == TextureRole::normal &&
         asset.color_space != TextureAssetColorSpace::linear)) {
        return core::Status::failure("texture_asset.invalid_asset",
                                     "texture asset header or role is invalid");
    }
    auto width = asset.width;
    auto height = asset.height;
    std::size_t total_bytes = 0;
    for (const auto& mip : asset.mips) {
        const auto expected = asset.format == TextureAssetFormat::rgba8
                                  ? static_cast<std::size_t>(width) * height * 4U
                                  : block_level_bytes(width, height);
        if (mip.width != width || mip.height != height || mip.bytes.size() != expected ||
            expected > limits.maximum_payload_bytes - total_bytes) {
            return core::Status::failure("texture_asset.invalid_mip",
                                         "texture asset mip dimensions or byte count are invalid");
        }
        total_bytes += expected;
        width = std::max(1U, width / 2U);
        height = std::max(1U, height / 2U);
    }
    if (asset.mips.size() > 1U &&
        (asset.mips.back().width != 1U || asset.mips.back().height != 1U)) {
        return core::Status::failure("texture_asset.incomplete_mips",
                                     "texture asset mip chains must terminate at one texel");
    }
    return core::Status::ok();
}

core::Result<std::vector<std::uint8_t>> encode_texture_asset(const TextureAsset& asset,
                                                             const TextureAssetLimits& limits) {
    auto status = validate_texture_asset(asset, limits);
    if (!status) {
        return core::Result<std::vector<std::uint8_t>>::failure(status.error().code,
                                                                status.error().message);
    }
    ByteWriter writer;
    writer.string(texture_magic);
    writer.u32(asset.width);
    writer.u32(asset.height);
    writer.u8(static_cast<std::uint8_t>(asset.role));
    writer.u8(static_cast<std::uint8_t>(asset.color_space));
    writer.u8(static_cast<std::uint8_t>(asset.format));
    writer.u8(asset.alpha_coverage_preserved ? 1U : 0U);
    writer.f32(asset.alpha_cutoff);
    writer.u32(static_cast<std::uint32_t>(asset.mips.size()));
    for (const auto& mip : asset.mips) {
        writer.u32(mip.width);
        writer.u32(mip.height);
        writer.u32(static_cast<std::uint32_t>(mip.bytes.size()));
        writer.bytes(mip.bytes);
    }
    auto bytes = writer.take();
    if (bytes.size() > limits.maximum_payload_bytes) {
        return core::Result<std::vector<std::uint8_t>>::failure(
            "texture_asset.payload_limit",
            "encoded texture asset exceeds its configured payload limit");
    }
    return core::Result<std::vector<std::uint8_t>>::success(std::move(bytes));
}

core::Result<TextureAsset> decode_texture_asset(std::span<const std::uint8_t> bytes,
                                                const TextureAssetLimits& limits) {
    if (bytes.empty() || bytes.size() > limits.maximum_payload_bytes) {
        return core::Result<TextureAsset>::failure(
            "texture_asset.payload_limit",
            "texture asset payload is empty or exceeds its configured limit");
    }
    ByteReader reader(bytes);
    auto magic = reader.string(texture_magic.size());
    if (!magic || magic.value() != texture_magic) {
        return core::Result<TextureAsset>::failure("texture_asset.invalid_magic",
                                                   "texture asset payload has an unknown version");
    }
    auto width = reader.u32();
    auto height = reader.u32();
    auto role = reader.u8();
    auto color_space = reader.u8();
    auto format = reader.u8();
    auto flags = reader.u8();
    auto alpha_cutoff = reader.f32();
    auto mip_count = reader.u32();
    if (!width || !height || !role || !color_space || !format || !flags || !alpha_cutoff ||
        !mip_count) {
        return core::Result<TextureAsset>::failure("texture_asset.truncated",
                                                   "texture asset header is truncated");
    }
    if (role.value() > static_cast<std::uint8_t>(TextureRole::data) ||
        color_space.value() > static_cast<std::uint8_t>(TextureAssetColorSpace::srgb) ||
        format.value() > static_cast<std::uint8_t>(TextureAssetFormat::bc7_rgba) ||
        (flags.value() & static_cast<std::uint8_t>(~1U)) != 0U || mip_count.value() == 0 ||
        mip_count.value() > limits.maximum_mip_levels) {
        return core::Result<TextureAsset>::failure(
            "texture_asset.invalid_header", "texture asset enum, flag, or mip count is invalid");
    }
    TextureAsset asset;
    asset.width = width.value();
    asset.height = height.value();
    asset.role = static_cast<TextureRole>(role.value());
    asset.color_space = static_cast<TextureAssetColorSpace>(color_space.value());
    asset.format = static_cast<TextureAssetFormat>(format.value());
    asset.alpha_coverage_preserved = flags.value() != 0U;
    asset.alpha_cutoff = alpha_cutoff.value();
    asset.mips.resize(mip_count.value());
    for (auto& mip : asset.mips) {
        auto mip_width = reader.u32();
        auto mip_height = reader.u32();
        auto byte_count = reader.u32();
        if (!mip_width || !mip_height || !byte_count ||
            byte_count.value() > limits.maximum_payload_bytes) {
            return core::Result<TextureAsset>::failure(
                "texture_asset.truncated", "texture asset mip header is truncated or too large");
        }
        auto mip_bytes = reader.bytes(byte_count.value());
        if (!mip_bytes) {
            return core::Result<TextureAsset>::failure(mip_bytes.error().code,
                                                       mip_bytes.error().message);
        }
        mip.width = mip_width.value();
        mip.height = mip_height.value();
        mip.bytes.assign(mip_bytes.value().begin(), mip_bytes.value().end());
    }
    if (!reader.at_end()) {
        return core::Result<TextureAsset>::failure("texture_asset.trailing_data",
                                                   "texture asset payload contains trailing data");
    }
    auto status = validate_texture_asset(asset, limits);
    if (!status) {
        return core::Result<TextureAsset>::failure(status.error().code, status.error().message);
    }
    return core::Result<TextureAsset>::success(std::move(asset));
}

std::string_view texture_role_name(TextureRole role) noexcept {
    switch (role) {
    case TextureRole::color:
        return "color";
    case TextureRole::normal:
        return "normal";
    case TextureRole::metallic_roughness:
        return "metallic_roughness";
    case TextureRole::occlusion:
        return "occlusion";
    case TextureRole::emissive:
        return "emissive";
    case TextureRole::mask:
        return "mask";
    case TextureRole::user_interface:
        return "ui";
    case TextureRole::data:
        return "data";
    }
    return "unknown";
}

std::string_view texture_color_space_name(TextureAssetColorSpace color_space) noexcept {
    return color_space == TextureAssetColorSpace::srgb ? "srgb" : "linear";
}

std::string_view texture_asset_format_name(TextureAssetFormat format) noexcept {
    switch (format) {
    case TextureAssetFormat::rgba8:
        return "rgba8";
    case TextureAssetFormat::bc5_rg:
        return "bc5_rg";
    case TextureAssetFormat::bc7_rgba:
        return "bc7_rgba";
    }
    return "unknown";
}

} // namespace heartstead::assets
