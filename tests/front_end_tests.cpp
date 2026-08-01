#include "engine/save/save_slot.hpp"
#include "game/application/application_settings.hpp"
#include "game/application/main_menu.hpp"

#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

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
    settings.rendering_quality = heartstead::renderer::RendererQualityPreset::ultra;
    settings.master_volume = 0.75F;
    settings.music_volume = 0.25F;
    settings.effects_volume = 0.5F;
    settings.mouse_sensitivity = 1.25F;
    settings.controller_sensitivity = 1.5F;
    settings.controller_enabled = false;
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
    assert(loaded.value().rendering_quality == heartstead::renderer::RendererQualityPreset::ultra);
    assert(loaded.value().master_volume == 0.75F);
    assert(loaded.value().controller_sensitivity == 1.5F);
    assert(loaded.value().color_vision_mode ==
           heartstead::renderer::UiColorVisionMode::deuteranopia);
    assert(loaded.value().last_world_slot == "homestead");
    assert(loaded.value().recent_servers.size() == 2);

    settings.window_width = 10;
    const auto invalid = store.save(settings);
    assert(!invalid);
    assert(invalid.error().code == "application_settings.invalid_resolution");
}

void test_save_world_management() {
    TemporaryDirectory temporary;
    save::FileSaveSlotCatalog catalog(temporary.path() / "saves");
    assert(catalog.create_slot("world_a"));

    save::SaveSnapshot snapshot;
    snapshot.metadata.game_version = "0.1.0";
    snapshot.metadata.world_seed = 42;
    assert(catalog.write_snapshot("world_a", snapshot, 100));
    assert(catalog.rename_slot("world_a", "World A"));
    assert(catalog.duplicate_slot("world_a", "world_a_copy", "World A Copy", 200));

    auto listed = catalog.list_slots();
    assert(listed);
    assert(listed.value().size() == 2);
    assert(listed.value()[0].metadata.display_name == "World A");
    assert(listed.value()[0].snapshot_metadata.has_value());
    assert(listed.value()[1].metadata.display_name == "World A Copy");
    assert(listed.value()[1].snapshot_metadata.has_value());

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

} // namespace

int main() {
    test_main_menu_navigation();
    test_application_settings_round_trip();
    test_save_world_management();
    return 0;
}
