#pragma once

#include "heartstead/core/math.hpp"
#include "heartstead/render/menu_ui.hpp"
#include "heartstead/render/video_settings.hpp"
#include "heartstead/voxel/mesh.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace heartstead {

struct VideoSettingsUiState {
    bool editing_render_distance{};
    std::string numeric_text;
};

struct DebugStats {
    double frames_per_second{};
    double cpu_frame_milliseconds{};
    double cpu_usage_percent{};
    double gpu_frame_milliseconds{};
    double gpu_load_percent{};
    std::size_t world_storage_bytes{};
    std::size_t gpu_storage_bytes{};
    std::uint32_t visible_chunks{};
    std::uint32_t occluded_chunks{};
    std::uint32_t total_chunks{};
};

struct VideoSettingsLayout {
    std::int32_t panel_left{};
    std::int32_t panel_top{};
    std::int32_t panel_right{};
    std::int32_t panel_bottom{};
    std::int32_t slider_left{};
    std::int32_t slider_right{};
    std::int32_t render_distance_y{};
    std::int32_t smoothing_y{};
    std::int32_t fog_start_y{};
    std::int32_t shadow_distance_y{};
    std::int32_t vsync_y{};
    std::int32_t fullscreen_y{};
    std::int32_t toggle_left{};
    std::int32_t toggle_right{};
    std::int32_t render_value_left{};
    std::int32_t render_value_top{};
    std::int32_t render_value_right{};
    std::int32_t render_value_bottom{};

    [[nodiscard]] static VideoSettingsLayout from_framebuffer(std::int32_t width, std::int32_t height) noexcept;
};

class OpenGlRenderer {
public:
    OpenGlRenderer(const WorldMesh& mesh, Float3 world_origin);
    ~OpenGlRenderer();

    OpenGlRenderer(const OpenGlRenderer&) = delete;
    OpenGlRenderer& operator=(const OpenGlRenderer&) = delete;

    void render(
        const Matrix4& view_projection,
        Float3 camera_position,
        const VideoSettings& settings,
        std::int32_t width,
        std::int32_t height,
        bool show_player,
        bool show_crosshair,
        Float3 player_position,
        float player_yaw);
    void render_video_settings(
        const VideoSettings& settings,
        const VideoSettingsUiState& ui_state,
        std::int32_t width,
        std::int32_t height);
    void render_menu(
        const MenuUiState& state,
        const VideoSettings& settings,
        std::int32_t width,
        std::int32_t height);
    void render_debug_overlay(const DebugStats& stats, std::int32_t width, std::int32_t height);
    bool apply_chunk_updates(
        const std::vector<ChunkMeshUpdate>& updates,
        const std::vector<Int3>& removals);
    [[nodiscard]] bool has_chunk(Int3 coordinates) const noexcept;
    [[nodiscard]] std::size_t gpu_storage_bytes() const noexcept;
    [[nodiscard]] double gpu_frame_milliseconds() const noexcept;

    [[nodiscard]] std::uint32_t visible_chunk_count() const noexcept { return visible_chunk_count_; }
    [[nodiscard]] std::uint32_t occluded_chunk_count() const noexcept { return occluded_chunk_count_; }
    [[nodiscard]] std::uint32_t total_chunk_count() const noexcept {
        return static_cast<std::uint32_t>(draw_ranges_.size());
    }

private:
    struct StreamingStorage;
    struct OcclusionStorage;
    struct PlayerStorage;
    struct ShadowStorage;
    struct CrosshairStorage;
    struct GpuTimingStorage;
    std::uint32_t opaque_vertex_array_{};
    std::uint32_t cutout_vertex_array_{};
    std::uint32_t vertex_buffer_{};
    std::uint32_t opaque_index_buffer_{};
    std::uint32_t cutout_index_buffer_{};
    std::uint32_t program_{};
    std::uint32_t ui_program_{};
    std::uint32_t ui_texture_{};
    std::uint32_t menu_texture_{};
    std::uint32_t debug_texture_{};
    std::int32_t view_projection_location_{-1};
    std::int32_t light_view_projection_location_{-1};
    std::int32_t player_light_view_projection_location_{-1};
    std::int32_t world_origin_location_{-1};
    std::int32_t camera_position_location_{-1};
    std::int32_t shadow_caster_position_location_{-1};
    std::int32_t distance_smoothing_start_location_{-1};
    std::int32_t fog_start_fraction_location_{-1};
    std::int32_t render_distance_blocks_location_{-1};
    std::int32_t shadow_distance_location_{-1};
    std::int32_t shadow_map_location_{-1};
    std::int32_t player_shadow_map_location_{-1};
    std::int32_t ui_sampler_location_{-1};
    Float3 world_origin_{};
    std::vector<ChunkDrawRange> draw_ranges_;
    std::vector<std::uint32_t> visible_ranges_;
    std::vector<std::int32_t> opaque_draw_counts_;
    std::vector<const void*> opaque_draw_offsets_;
    std::vector<std::int32_t> cutout_draw_counts_;
    std::vector<const void*> cutout_draw_offsets_;
    std::vector<std::int32_t> shadow_opaque_draw_counts_;
    std::vector<const void*> shadow_opaque_draw_offsets_;
    std::vector<std::int32_t> shadow_cutout_draw_counts_;
    std::vector<const void*> shadow_cutout_draw_offsets_;
    std::uint32_t visible_chunk_count_{};
    std::uint32_t occluded_chunk_count_{};
    std::unique_ptr<StreamingStorage> streaming_storage_;
    std::unique_ptr<OcclusionStorage> occlusion_storage_;
    std::unique_ptr<PlayerStorage> player_storage_;
    std::unique_ptr<ShadowStorage> shadow_storage_;
    std::unique_ptr<CrosshairStorage> crosshair_storage_;
    std::unique_ptr<GpuTimingStorage> gpu_timing_storage_;
    std::int32_t ui_cached_width_{};
    std::int32_t ui_cached_height_{};
    std::int32_t ui_cached_render_distance_{-1};
    float ui_cached_smoothing_{-1.0F};
    float ui_cached_fog_start_{-1.0F};
    std::int32_t ui_cached_shadow_distance_{-1};
    std::int32_t ui_cached_scale_max_{-1};
    bool ui_cached_vsync_{};
    bool ui_cached_fullscreen_{};
    bool ui_cached_editing_{};
    std::string ui_cached_numeric_text_;
    std::vector<std::uint8_t> ui_pixels_;
    std::int32_t menu_cached_width_{};
    std::int32_t menu_cached_height_{};
    MenuScreen menu_cached_screen_{MenuScreen::main};
    std::int32_t menu_cached_hovered_{-2};
    std::int32_t menu_cached_selected_world_{-2};
    std::int32_t menu_cached_render_distance_{-1};
    bool menu_cached_editing_{};
    std::string menu_cached_world_name_;
    std::string menu_cached_creation_date_;
    std::string menu_cached_multiplayer_status_;
    std::vector<SavedWorldUiEntry> menu_cached_saved_worlds_;
    std::vector<std::uint8_t> menu_pixels_;
    std::int32_t debug_cached_width_{};
    std::int32_t debug_cached_height_{};
    std::string debug_cached_text_;
    std::vector<std::uint8_t> debug_pixels_;
};

} // namespace heartstead
