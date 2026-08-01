#pragma once

#include "engine/entities/entity.hpp"
#include "engine/items/item_stack.hpp"
#include "engine/movement/player_controller.hpp"
#include "engine/platform/platform.hpp"
#include "engine/player_profiles/map_discovery.hpp"
#include "engine/ui/map_view.hpp"
#include "engine/ui/widget_tree.hpp"
#include "engine/world/world_state.hpp"
#include "game/runtime/client_runtime.hpp"
#include "game/runtime/runtime_session.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace heartstead::game {

struct InventoryUiPendingTransfer {
    std::uint64_t command_sequence = 0;
    world::InventoryTransferRequest request;
};

struct InventoryUiSynchronizationStats {
    std::uint32_t confirmed_transfer_count = 0;
    std::uint32_t rejected_transfer_count = 0;
    std::uint32_t reapplied_optimistic_transfer_count = 0;
    std::uint32_t rolled_back_transfer_count = 0;
};

class InventoryUiViewModel {
  public:
    explicit InventoryUiViewModel(std::span<const items::ItemDefinition> item_definitions = {});

    [[nodiscard]] core::Result<InventoryUiSynchronizationStats>
    synchronize(const ClientRuntime& client);
    [[nodiscard]] core::Result<std::uint64_t>
    request_transfer(RuntimeSession& runtime, const world::InventoryTransferRequest& request,
                     std::int64_t now_ms = 0);

    [[nodiscard]] core::SaveId local_owner_id() const noexcept;
    [[nodiscard]] const world::InventoryRecord* displayed_inventory() const noexcept;
    [[nodiscard]] std::span<const InventoryUiPendingTransfer> pending_transfers() const noexcept;
    [[nodiscard]] const std::optional<core::Error>& last_rejection() const noexcept;
    [[nodiscard]] std::uint64_t total_mass_grams() const noexcept;
    void clear_rejection() noexcept;

  private:
    [[nodiscard]] core::Status rebuild_optimistic_inventory(
        const world::InventoryRecord& authoritative,
        InventoryUiSynchronizationStats& stats);
    [[nodiscard]] std::uint64_t calculate_total_mass_grams() const noexcept;

    std::span<const items::ItemDefinition> item_definitions_;
    core::SaveId local_owner_id_;
    std::optional<world::InventoryRecord> displayed_inventory_;
    std::vector<InventoryUiPendingTransfer> pending_;
    std::optional<core::Error> last_rejection_;
    std::uint64_t total_mass_grams_ = 0;
};

struct GameUiProcessResult {
    ui::UiInputConsumption consumed;
    std::uint32_t event_count = 0;
    std::optional<std::uint64_t> submitted_inventory_command;
    bool inventory_toggled = false;
    bool map_toggled = false;
};

struct GameUiStats {
    ui::UiLayoutStats layout;
    ui::UiPaintStats paint;
    InventoryUiSynchronizationStats inventory;
    std::uint32_t pending_inventory_transfers = 0;
    std::uint64_t inventory_mass_grams = 0;
    double layout_ms = 0.0;
    double paint_ms = 0.0;
    bool inventory_open = false;
    bool map_open = false;
    ui::MapViewStats minimap;
    ui::MapViewStats full_map;
};

class GameUiLayer {
  public:
    GameUiLayer(std::span<const items::ItemDefinition> item_definitions,
                std::span<const entities::EntityDefinition> entity_definitions,
                ui::UiSkin skin = ui::UiSkin::storybook_default());

    [[nodiscard]] core::Status initialize();
    [[nodiscard]] core::Result<InventoryUiSynchronizationStats>
    synchronize(const ClientRuntime& client);
    [[nodiscard]] core::Result<GameUiProcessResult>
    process_input(const platform::WindowInputSnapshot& input, RuntimeSession& runtime,
                  std::int64_t now_ms = 0);
    [[nodiscard]] core::Result<ui::UiPaintStats>
    paint(renderer::UiRenderer& renderer, renderer::rhi::RenderExtent extent,
          float dpi_scale = 1.0F);

    void set_inventory_open(bool open) noexcept;
    void set_map_open(bool open) noexcept;
    void set_map_center(player_profiles::MapCellCoord center) noexcept;
    [[nodiscard]] core::Status set_map_layer(std::string layer_id);
    void set_map_markers(std::vector<ui::MapMarker> markers);
    void set_map_discovery(player_profiles::MapDiscovery discovery);
    [[nodiscard]] bool inventory_open() const noexcept;
    [[nodiscard]] bool map_open() const noexcept;
    [[nodiscard]] bool blocks_gameplay() const noexcept;
    [[nodiscard]] InventoryUiViewModel& inventory() noexcept;
    [[nodiscard]] const InventoryUiViewModel& inventory() const noexcept;
    [[nodiscard]] ui::WidgetTree& widgets() noexcept;
    [[nodiscard]] const ui::WidgetTree& widgets() const noexcept;
    [[nodiscard]] const GameUiStats& stats() const noexcept;

  private:
    [[nodiscard]] core::Status build_widgets();
    [[nodiscard]] core::Status update_inventory_widgets();
    [[nodiscard]] core::Status update_hud_widgets(const ClientRuntime& client);
    [[nodiscard]] core::Result<std::optional<std::uint64_t>>
    handle_widget_events(std::span<const ui::UiEvent> events, bool split_stack,
                         RuntimeSession& runtime, std::int64_t now_ms);
    [[nodiscard]] std::optional<std::size_t> inventory_slot_from_widget(ui::WidgetId id) const;
    [[nodiscard]] std::uint64_t player_capacity_grams(const ClientRuntime& client) const noexcept;

    std::span<const items::ItemDefinition> item_definitions_;
    std::span<const entities::EntityDefinition> entity_definitions_;
    InventoryUiViewModel inventory_;
    ui::WidgetTree widgets_;
    ui::MapViewRenderer map_view_renderer_;
    player_profiles::MapDiscovery map_discovery_;
    std::vector<ui::MapMarker> map_markers_;
    player_profiles::MapCellCoord map_center_{};
    std::string map_layer_ = "surface";
    std::vector<ui::WidgetId> inventory_slots_;
    std::vector<ui::WidgetId> hotbar_slots_;
    GameUiStats stats_{};
    std::uint64_t player_capacity_grams_ = 1;
    bool initialized_ = false;
    bool inventory_open_ = false;
    bool map_open_ = false;
};

} // namespace heartstead::game
