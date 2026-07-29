#pragma once

#include "engine/core/result.hpp"
#include "engine/renderer/particles/particle_system.hpp"
#include "engine/renderer/renderer.hpp"

#include <array>
#include <cstdint>
#include <unordered_map>

namespace heartstead::game {

struct ParticlePresentationConfig {
    std::array<renderer::MaterialRuntimeHandle, 4> material_groups{};
    std::uint32_t maximum_presented_particles = 50'000;

    [[nodiscard]] core::Status validate() const noexcept;
};

struct ParticlePresentationStats {
    std::uint32_t retained_particles = 0;
    std::uint32_t inserted_particles = 0;
    std::uint32_t updated_particles = 0;
    std::uint32_t removed_particles = 0;
    std::uint32_t material_groups = 0;
    std::uint64_t dropped_particles = 0;
    double synchronize_ms = 0.0;
};

class ParticlePresentation {
  public:
    [[nodiscard]] core::Status initialize(renderer::Renderer& renderer,
                                          ParticlePresentationConfig config = {});
    [[nodiscard]] core::Result<ParticlePresentationStats>
    synchronize(renderer::Renderer& renderer, const renderer::CpuParticleSystem& particles,
                const renderer::RenderCamera& camera);
    [[nodiscard]] core::Status shutdown(renderer::Renderer& renderer);

    [[nodiscard]] bool is_initialized() const noexcept;
    [[nodiscard]] const ParticlePresentationStats& stats() const noexcept;

  private:
    struct RetainedParticle {
        renderer::RenderObjectId object;
    };

    ParticlePresentationConfig config_{};
    renderer::RenderMeshHandle billboard_mesh_{};
    std::unordered_map<std::uint64_t, RetainedParticle> retained_;
    ParticlePresentationStats stats_{};
    bool initialized_ = false;
};

} // namespace heartstead::game
