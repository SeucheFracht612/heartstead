#include "engine/renderer/environment/environment_lighting.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace heartstead::renderer {

EnvironmentLighting::EnvironmentLighting(rhi::IRenderDevice& device) : device_(&device) {}

EnvironmentLighting::~EnvironmentLighting() {
    (void)shutdown();
}

core::Status EnvironmentLighting::initialize() {
    if (image_.is_valid()) {
        return core::Status::failure("environment_lighting.already_initialized",
                                     "environment lighting cannot be initialized twice");
    }
    constexpr std::uint32_t size = 32;
    constexpr std::uint32_t mip_levels = 6;
    constexpr std::array<std::array<float, 3>, 6> face_colors{{
        {0.26F, 0.38F, 0.62F}, {0.26F, 0.38F, 0.62F},
        {0.45F, 0.60F, 0.88F}, {0.08F, 0.075F, 0.07F},
        {0.24F, 0.36F, 0.62F}, {0.24F, 0.36F, 0.62F},
    }};
    constexpr std::array<float, 3> average{0.255F, 0.365F, 0.57F};
    std::vector<std::byte> bytes;
    for (std::uint32_t mip = 0; mip < mip_levels; ++mip) {
        const auto mip_size = std::max(size >> mip, 1U);
        const auto roughness = static_cast<float>(mip) /
                               static_cast<float>(mip_levels - 1U);
        for (const auto& face : face_colors) {
            for (std::uint32_t pixel = 0; pixel < mip_size * mip_size; ++pixel) {
                for (std::size_t channel = 0; channel < 3; ++channel) {
                    const auto linear =
                        face[channel] * (1.0F - roughness) + average[channel] * roughness;
                    bytes.push_back(static_cast<std::byte>(
                        static_cast<std::uint8_t>(std::clamp(linear, 0.0F, 1.0F) * 255.0F)));
                }
                bytes.push_back(std::byte{0xff});
            }
        }
    }
    rhi::RenderImageDesc image_desc;
    image_desc.format = rhi::RenderImageFormat::rgba8_unorm;
    image_desc.width = size;
    image_desc.height = size;
    image_desc.array_layers = 6;
    image_desc.mip_levels = mip_levels;
    image_desc.cubemap = true;
    image_desc.debug_name = "default_prefiltered_environment";
    auto image = device_->upload_image(std::move(image_desc), bytes);
    if (!image) {
        return core::Status::failure(image.error().code, image.error().message);
    }
    image_ = image.value().handle;
    rhi::RenderSamplerDesc sampler_desc;
    sampler_desc.min_filter = rhi::RenderSamplerFilter::linear;
    sampler_desc.mag_filter = rhi::RenderSamplerFilter::linear;
    sampler_desc.mipmap_mode = rhi::RenderSamplerMipmapMode::linear;
    sampler_desc.address_u = rhi::RenderSamplerAddressMode::clamp_to_edge;
    sampler_desc.address_v = rhi::RenderSamplerAddressMode::clamp_to_edge;
    sampler_desc.address_w = rhi::RenderSamplerAddressMode::clamp_to_edge;
    sampler_desc.max_lod = static_cast<float>(mip_levels - 1U);
    sampler_desc.debug_name = "environment_sampler";
    auto sampler = device_->create_sampler(std::move(sampler_desc));
    if (!sampler) {
        const auto error = sampler.error();
        (void)device_->release_resource(image_);
        image_ = {};
        return core::Status::failure(error.code, error.message);
    }
    sampler_ = sampler.value().handle;
    return core::Status::ok();
}

core::Status EnvironmentLighting::bind(core::PrototypeId material, std::string_view binding) {
    const rhi::RenderDescriptorWrite write{material, std::string(binding), image_, 0, 0, sampler_};
    auto result = device_->write_descriptors(std::span{&write, 1});
    return result ? core::Status::ok()
                  : core::Status::failure(result.error().code, result.error().message);
}

core::Status EnvironmentLighting::shutdown() {
    auto status = core::Status::ok();
    if (sampler_.is_valid()) {
        status = device_->release_resource(sampler_);
        sampler_ = {};
    }
    if (image_.is_valid()) {
        auto released = device_->release_resource(image_);
        if (!released && status) {
            status = released;
        }
        image_ = {};
    }
    return status;
}

rhi::RenderResourceHandle EnvironmentLighting::image() const noexcept {
    return image_;
}

} // namespace heartstead::renderer
