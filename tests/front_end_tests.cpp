#include "engine/save/save_slot.hpp"
#include "game/application/application_settings.hpp"
#include "game/application/launch_options.hpp"
#include "game/application/main_menu.hpp"
#include "game/application/runtime_diagnostics.hpp"

#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

namespace {

namespace game = heartstead::game;
namespace save = heartstead::save;

class TemporaryDirectory final {
  public:
    TemporaryDirectory() {
        const auto suffix = std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = std::filesystem::temp_directory_path() /
                ("heartstead_front_end_tests_" + std::to_string(suffix));
        std::filesystem::create_directories(path_);
    }

    ~TemporaryDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }

    TemporaryDirectory(const TemporaryDirectory&) = delete;
    TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;

    [[nodiscard]] const std::filesystem::path& path() const noexcept {
        return path_;
    }

  private:
    std::filesystem::path path_;
};

void test_main_menu_navigation() {
    game::MainMenuNavigation navigation;
    assert(navigation.screen() == game::MainMenuScreen::root);

    const auto disabled = game::MainMenuNavigation::root_actions(false);
    assert(disabled.size() == 7);
    assert(disabled.front().action == game::MainMenuAction::continue_world);
    assert(!disabled.front().enabled);
    assert(!disabled.front().disabled_reason.empty());
    for (std::size_t index = 1; index < disabled.size(); ++index) {
        assert(disabled[index].enabled);
    }

    auto status = navigation.open(game::MainMenuScreen::delete_confirmation);
    assert(!status);
    assert(status.error().code == "main_menu.invalid_navigation");
    assert(navigation.open(game::MainMenuScreen::load_world));
    assert(navigation.open(game::MainMenuScreen::delete_confirmation));
    assert(navigation.back());
    assert(navigation.screen() == game::MainMenuScreen::load_world);
    assert(navigation.open(game::MainMenuScreen::rename_world));
    assert(navigation.back());
    assert(navigation.screen() == game::MainMenuScreen::load_world);
    assert(navigation.back());
    assert(navigation.screen() == game::MainMenuScreen::root);
    assert(!navigation.back());

    assert(navigation.open(game::MainMenuScreen::options));
    status = navigation.open(game::MainMenuScreen::multiplayer);
    assert(!status);
    assert(navigation.back());
}

void test_application_settings_round_trip() {
    TemporaryDirectory temporary;
    const game::ApplicationSettingsStore store(temporary.path() / "settings.txt");

    auto loaded = store.load();
    assert(loaded);
    assert(loaded.value().window_width == 1280);

    game::ApplicationSettings settings;
    settings.window_width = 1920;
    settings.window_height = 1080;
    settings.windowed = false;
    settings.vsync = false;
    settings.first_person_camera = true;
    settings.rendering_quality = heartstead::renderer::RendererQualityPreset::ultra;
    settings.master_volume = 0.75F;
    settings.music_volume = 0.25F;
    settings.effects_volume = 0.5F;
    settings.mouse_sensitivity = 1.25F;
    settings.ui_scale = 1.5F;
    settings.ui_contrast = 1.25F;
    settings.ui_saturation = 0.8F;
    settings.color_vision_mode = heartstead::renderer::UiColorVisionMode::deuteranopia;
    settings.reduced_motion = true;
    settings.last_world_slot = "homestead";
    settings.recent_servers = {"127.0.0.1:7777", "example.test:27015"};
    assert(store.save(settings));

    loaded = store.load();
    assert(loaded);
    assert(loaded.value().window_width == 1920);
    assert(!loaded.value().windowed);
    assert(!loaded.value().vsync);
    assert(loaded.value().first_person_camera);
    assert(loaded.value().rendering_quality == heartstead::renderer::RendererQualityPreset::ultra);
    assert(loaded.value().master_volume == 0.75F);
    assert(loaded.value().color_vision_mode ==
           heartstead::renderer::UiColorVisionMode::deuteranopia);
    assert(loaded.value().last_world_slot == "homestead");
    assert(loaded.value().recent_servers.size() == 2);

    settings.window_width = 10;
    const auto invalid = store.save(settings);
    assert(!invalid);
    assert(invalid.error().code == "application_settings.invalid_resolution");

    settings.window_width = 1920;
    assert(store.save(settings));
    std::ifstream input(store.path(), std::ios::binary);
    std::string serialized((std::istreambuf_iterator<char>(input)),
                           std::istreambuf_iterator<char>());
    const auto volume = serialized.find("master_volume|0.75");
    assert(volume != std::string::npos);
    serialized.replace(volume, std::string("master_volume|0.75").size(),
                       "master_volume|not-a-number");
    const auto end = serialized.find("end\n");
    assert(end != std::string::npos);
    serialized.insert(end, "future_setting|preserved-by-newer-version\n");
    {
        std::ofstream output(store.path(), std::ios::binary | std::ios::trunc);
        output << serialized;
        assert(output);
    }
    loaded = store.load();
    assert(loaded);
    assert(loaded.value().master_volume == 1.0F);
    assert(loaded.value().window_width == 1920);

    const auto vsync = serialized.find("vsync|false\n");
    assert(vsync != std::string::npos);
    serialized.erase(vsync, std::string("vsync|false\n").size());
    {
        std::ofstream output(store.path(), std::ios::binary | std::ios::trunc);
        output << serialized;
        assert(output);
    }
    loaded = store.load();
    assert(!loaded);
    assert(loaded.error().code == "application_settings.incomplete");

    assert(store.save(settings));
    {
        std::ifstream camera_input(store.path(), std::ios::binary);
        serialized.assign(std::istreambuf_iterator<char>(camera_input),
                          std::istreambuf_iterator<char>());
    }
    const auto camera = serialized.find("first_person_camera|true\n");
    assert(camera != std::string::npos);
    serialized.erase(camera, std::string("first_person_camera|true\n").size());
    {
        std::ofstream output(store.path(), std::ios::binary | std::ios::trunc);
        output << serialized;
        assert(output);
    }
    loaded = store.load();
    assert(!loaded);
    assert(loaded.error().code == "application_settings.incomplete");

    {
        std::ofstream output(store.path(), std::ios::binary | std::ios::trunc);
        output << "heartstead.application_settings.v1\n"
               << "windowed|false\n"
               << "window_width|broken\n"
               << "end\n";
        assert(output);
    }
    loaded = store.load();
    assert(loaded);
    assert(!loaded.value().windowed);
    assert(loaded.value().window_width == 1280);
    assert(loaded.value().vsync);

    {
        std::ofstream output(store.path(), std::ios::binary | std::ios::trunc);
        output << "heartstead.application_settings.v2\n"
               << "windowed|false\n"
               << "controller_enabled|false\n"
               << "controller_sensitivity|2.5\n"
               << "end\n";
        assert(output);
    }
    loaded = store.load();
    assert(loaded);
    assert(!loaded.value().windowed);
    assert(store.save(loaded.value()));
    {
        std::ifstream migrated_input(store.path(), std::ios::binary);
        serialized.assign(std::istreambuf_iterator<char>(migrated_input),
                          std::istreambuf_iterator<char>());
    }
    assert(serialized.starts_with("heartstead.application_settings.v3\n"));
    assert(serialized.find("controller_enabled|") == std::string::npos);
    assert(serialized.find("controller_sensitivity|") == std::string::npos);
}

void test_ui_scale_adapts_to_window_capacity() {
    assert(game::effective_application_ui_scale(1280, 720, 2.0F) == 2.0F);
    assert(game::effective_application_ui_scale(640, 360, 2.0F) == 1.0F);
    assert(game::effective_application_ui_scale(800, 450, 1.5F) == 1.25F);
    assert(game::effective_application_ui_scale(320, 180, 2.0F) == 0.75F);
    assert(game::effective_application_ui_scale(1920, 1080, 0.8F) == 0.8F);
}

void test_save_world_management() {
    TemporaryDirectory temporary;
    save::FileSaveSlotCatalog catalog(temporary.path() / "saves");
    assert(catalog.create_slot("world_a"));

    save::SaveSnapshot snapshot;
    snapshot.metadata.game_version = "0.1.0";
    snapshot.metadata.world_seed = 42;
    snapshot.mod_states.push_back({"engine", "world.generator_preset", "base:temperate"});
    snapshot.mod_states.push_back({"engine", "world.generator_version", "7"});
    assert(catalog.write_snapshot("world_a", snapshot, 100));
    assert(catalog.rename_slot("world_a", "W\xC3\xB6rld A"));
    auto invalid_name = catalog.rename_slot("world_a", " World A");
    assert(!invalid_name && invalid_name.error().code == "save_slot.invalid_display_name");
    assert(catalog.duplicate_slot("world_a", "world_a_copy", "World A Copy", 200));

    auto listed = catalog.list_slots();
    assert(listed);
    assert(listed.value().size() == 2);
    assert(listed.value()[0].metadata.display_name == "W\xC3\xB6rld A");
    assert(listed.value()[0].snapshot_metadata.has_value());
    assert(listed.value()[0].generator_preset == "base:temperate");
    assert(listed.value()[0].generator_version == "7");
    assert(listed.value()[1].metadata.display_name == "World A Copy");
    assert(listed.value()[1].snapshot_metadata.has_value());
    assert(listed.value()[1].metadata.last_played_at_ms == 0);

    assert(catalog.delete_slot("world_a"));
    listed = catalog.list_slots();
    assert(listed);
    assert(listed.value().size() == 1);

    assert(catalog.create_slot("corrupt"));
    {
        std::ofstream output(temporary.path() / "saves" / "corrupt" / "slot.txt",
                             std::ios::binary | std::ios::trunc);
        output << "not a save slot\n";
        assert(output);
    }
    listed = catalog.list_slots();
    assert(listed);
    assert(listed.value().size() == 2);
    const auto& corrupt = listed.value().front();
    assert(corrupt.slot_id == "corrupt");
    assert(corrupt.validation_error.has_value());
}

void test_world_creation_input_helpers() {
    auto normalized = game::normalize_world_display_name("  H\xC3\xB6mestead  ");
    assert(normalized && normalized.value() == "H\xC3\xB6mestead");
    assert(!game::normalize_world_display_name("\x01invalid"));

    const auto spaced = game::world_slot_id("My World");
    const auto dashed = game::world_slot_id("My-World");
    const auto unicode = game::world_slot_id("\xE6\x88\x91\xE7\x9A\x84\xE4\xB8\x96\xE7\x95\x8C");
    assert(spaced != dashed);
    assert(save::FileSaveSlotCatalog::is_valid_slot_id(spaced));
    assert(save::FileSaveSlotCatalog::is_valid_slot_id(unicode));

    auto seed = game::parse_world_seed("42");
    assert(seed && seed.value() == 42);
    seed = game::parse_world_seed("0x2a");
    assert(seed && seed.value() == 42);
    seed = game::parse_world_seed("-42");
    assert(seed && seed.value() == static_cast<std::uint64_t>(std::int64_t{-42}));
    const auto text_seed = game::parse_world_seed("Heartstead");
    assert(text_seed && text_seed.value() == game::parse_world_seed("Heartstead").value());
    assert(game::parse_world_seed(" "));

    auto endpoint = game::parse_server_endpoint("127.0.0.1:7777");
    assert(endpoint && endpoint.value().address == "127.0.0.1" &&
           endpoint.value().port == 7777);
    endpoint = game::parse_server_endpoint("[::1]:27015");
    assert(endpoint && endpoint.value().address == "::1" && endpoint.value().port == 27015);
    assert(!game::parse_server_endpoint("::1:27015"));
    assert(!game::parse_server_endpoint("[::1]:0"));
}

void test_command_line_launch_contract() {
    using namespace std::string_view_literals;
    const std::string_view new_world[]{"--new-world"sv, "CLI Homestead"sv, "--seed"sv, "184467"sv,
                                       "--safe-mode"sv};
    auto parsed = game::parse_heartstead_launch_options(new_world);
    assert(parsed);
    assert(parsed.value().safe_mode);
    assert(parsed.value().initial_launch.has_value());
    assert(parsed.value().initial_launch->kind == game::InitialLaunchKind::new_world);
    assert(parsed.value().initial_launch->target == "CLI Homestead");
    assert(parsed.value().initial_launch->seed == 184467);

    const std::string_view world[]{"--world"sv, "/tmp/world-a"sv, "--headless"sv,
                                   "--native-frames"sv, "60"sv};
    parsed = game::parse_heartstead_launch_options(world);
    assert(parsed && parsed.value().headless && parsed.value().maximum_frames == 60);
    assert(parsed.value().initial_launch->kind == game::InitialLaunchKind::world);

    const std::string_view conflict[]{"--scenario"sv, "base:scenarios/renderer_proof"sv,
                                      "--connect"sv, "127.0.0.1:7777"sv};
    parsed = game::parse_heartstead_launch_options(conflict);
    assert(!parsed && parsed.error().code == "heartstead.conflicting_launch_options");

    const std::string_view invalid_seed[]{"--connect"sv, "127.0.0.1:7777"sv, "--seed"sv, "12"sv};
    parsed = game::parse_heartstead_launch_options(invalid_seed);
    assert(!parsed && parsed.error().code == "heartstead.seed_not_supported");

    const std::string_view help[]{"--help"sv};
    parsed = game::parse_heartstead_launch_options(help);
    assert(parsed && parsed.value().show_help);
    assert(game::heartstead_command_line_usage("heartstead").find("--scenario") !=
           std::string::npos);
}

void test_runtime_diagnostics_are_explicit() {
    game::RuntimeDiagnosticsSnapshot snapshot;
    snapshot.application_state = game::ApplicationState::in_game;
    snapshot.session_state = game::RuntimeSessionState::running;
    snapshot.session_mode = game::SessionMode::local_single_player;
    snapshot.connection_state = game::SessionConnectionState::connected;
    snapshot.active_world = "Diagnostics World";
    snapshot.session_generation = 9;
    snapshot.authoritative_tick = 120;
    snapshot.physics_objects = 4;
    snapshot.pending_chunk_load_operations = 2;
    snapshot.reserved_chunk_load_working_bytes = 128U * 1024U * 1024U;
    snapshot.last_chunk_load_worker_ms = 3.5;
    snapshot.maximum_chunk_load_pipeline_latency_ms = 8.25;
    snapshot.maximum_chunk_load_publication_us = 412;
    snapshot.pending_save_operations = 1;
    snapshot.reserved_save_working_bytes = 8U * 1024U * 1024U;
    snapshot.last_save_owner_handoff_ms = 0.2;
    snapshot.device_gpu_usage_bytes = 32U * 1024U * 1024U;
    snapshot.device_gpu_budget_bytes = 512U * 1024U * 1024U;
    const auto text = game::format_runtime_diagnostics(snapshot);
    assert(text.find("application InGame") != std::string::npos);
    assert(text.find("generation 9") != std::string::npos);
    assert(text.find("chunk load pending/reserved 2/128.0 MiB") != std::string::npos);
    assert(text.find("worker/pipeline 3.50/8.25 ms | publish max 412 us") !=
           std::string::npos);
    assert(text.find("save pending/reserved 1/8.0 MiB") != std::string::npos);
    assert(text.find("budget telemetry unavailable") == std::string::npos);

    const auto process = game::sample_process_resources();
#if defined(__linux__)
    assert(process.resident_memory_bytes.has_value());
    assert(process.thread_count.has_value() && *process.thread_count > 0);
    assert(process.open_file_count.has_value() && *process.open_file_count > 0);
#endif

    game::FrameRateCounter frame_rate;
    for (std::size_t frame = 0; frame < 15; ++frame) {
        frame_rate.record_frame(16'667);
    }
    assert(std::abs(frame_rate.sample().frames_per_second - 60.0) < 0.01);
    assert(game::format_frame_rate(frame_rate.sample()) == "FPS 60.0");
    frame_rate.reset();
    assert(frame_rate.sample().frames_per_second == 0.0);
}

} // namespace

int main() {
    test_main_menu_navigation();
    test_application_settings_round_trip();
    test_ui_scale_adapts_to_window_capacity();
    test_save_world_management();
    test_world_creation_input_helpers();
    test_command_line_launch_contract();
    test_runtime_diagnostics_are_explicit();
    return 0;
}
