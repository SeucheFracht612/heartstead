#include "game/ui/game_ui.hpp"

#include "engine/items/item_prototype.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <limits>
#include <ranges>
#include <string_view>
#include <utility>

namespace heartstead::game {

namespace {

constexpr std::size_t inventory_slot_count = 36;
constexpr std::size_t hotbar_slot_count = 9;
constexpr std::int32_t hud_vital_pool_milli = 100'000;

[[nodiscard]] bool key_pressed(const platform::WindowInputSnapshot& input,
                               platform::KeyCode key) noexcept {
    return std::ranges::find(input.pressed_keys, key) != input.pressed_keys.end();
}

[[nodiscard]] bool mouse_down(const platform::WindowInputSnapshot& input,
                              platform::MouseButton button) noexcept {
    return std::ranges::find(input.down_mouse_buttons, button) !=
           input.down_mouse_buttons.end();
}

[[nodiscard]] std::string item_label(const items::ItemStack& stack) {
    const auto local = stack.prototype_id.local_id();
    const auto slash = local.rfind('/');
    const auto name = slash == std::string_view::npos ? local : local.substr(slash + 1);
    return std::string(name) + "\nx" + std::to_string(stack.count);
}

[[nodiscard]] std::string item_tooltip(const items::ItemStack& stack,
                                       std::span<const items::ItemDefinition> definitions) {
    const auto definition =
        std::ranges::find(definitions, stack.prototype_id, &items::ItemDefinition::prototype_id);
    const auto mass = definition == definitions.end() ? 0 : definition->mass_grams * stack.count;
    return stack.prototype_id.value() + " | " + std::to_string(stack.count) + "/" +
           std::to_string(stack.max_count) + " | " + std::to_string(mass) + "g";
}

[[nodiscard]] std::optional<std::size_t> parse_slot_payload(std::string_view payload) noexcept {
    constexpr std::string_view prefix = "inventory-slot:";
    if (!payload.starts_with(prefix)) {
        return std::nullopt;
    }
    payload.remove_prefix(prefix.size());
    std::size_t value = 0;
    const auto [end, error] =
        std::from_chars(payload.data(), payload.data() + payload.size(), value);
    if (error != std::errc{} || end != payload.data() + payload.size()) {
        return std::nullopt;
    }
    return value;
}

[[nodiscard]] ui::WidgetDesc make_panel(ui::WidgetId id, ui::WidgetId parent) {
    ui::WidgetDesc result;
    result.id = id;
    result.parent = parent;
    result.kind = ui::WidgetKind::panel;
    result.nine_slice = "carved_panel";
    result.color = {0.29F, 0.15F, 0.055F, 0.95F};
    return result;
}

[[nodiscard]] core::Status update_widget(ui::WidgetTree& tree, ui::WidgetId id,
                                         const auto& update) {
    const auto* current = tree.find(id);
    if (current == nullptr) {
        return core::Status::failure("game_ui.missing_widget",
                                     "game UI widget is missing from the retained tree");
    }
    auto changed = *current;
    update(changed);
    return tree.update(std::move(changed));
}

} // namespace

InventoryUiViewModel::InventoryUiViewModel(
    std::span<const items::ItemDefinition> item_definitions)
    : item_definitions_(item_definitions) {}

core::Result<InventoryUiSynchronizationStats>
InventoryUiViewModel::synchronize(const ClientRuntime& client) {
    InventoryUiSynchronizationStats stats;
    const auto* player = client.local_player_snapshot();
    if (player == nullptr || !player->player_save_id.is_valid()) {
        return core::Result<InventoryUiSynchronizationStats>::failure(
            "inventory_ui.local_player_missing",
            "client does not contain the assigned local player identity");
    }
    if (local_owner_id_.is_valid() && local_owner_id_ != player->player_save_id) {
        pending_.clear();
        displayed_inventory_.reset();
    }
    local_owner_id_ = player->player_save_id;
    const auto* authoritative = client.world().inventories().find(local_owner_id_);
    if (authoritative == nullptr) {
        return core::Result<InventoryUiSynchronizationStats>::failure(
            "inventory_ui.inventory_missing",
            "client world does not contain the local player's replicated inventory");
    }

    for (const auto& result : client.command_results()) {
        const auto pending = std::ranges::find(pending_, result.sequence,
                                               &InventoryUiPendingTransfer::command_sequence);
        if (pending == pending_.end()) {
            continue;
        }
        if (result.success) {
            ++stats.confirmed_transfer_count;
        } else {
            ++stats.rejected_transfer_count;
            ++stats.rolled_back_transfer_count;
            last_rejection_ = core::Error{
                result.error_code.empty() ? "inventory_ui.transfer_rejected" : result.error_code,
                result.error_message.empty() ? "authoritative inventory transfer was rejected"
                                             : result.error_message};
        }
        pending_.erase(pending);
    }
    auto status = rebuild_optimistic_inventory(*authoritative, stats);
    if (!status) {
        return core::Result<InventoryUiSynchronizationStats>::failure(status.error().code,
                                                                       status.error().message);
    }
    total_mass_grams_ = calculate_total_mass_grams();
    return core::Result<InventoryUiSynchronizationStats>::success(stats);
}

core::Status InventoryUiViewModel::rebuild_optimistic_inventory(
    const world::InventoryRecord& authoritative, InventoryUiSynchronizationStats& stats) {
    displayed_inventory_ = authoritative;
    for (const auto& pending : pending_) {
        if (pending.request.source_owner_id != local_owner_id_ ||
            pending.request.destination_owner_id != local_owner_id_) {
            return core::Status::failure(
                "inventory_ui.cross_inventory_pending_unsupported",
                "local inventory view model can only replay local self-transfers");
        }
        auto status = world::transfer_inventory_items(*displayed_inventory_, *displayed_inventory_,
                                                      pending.request);
        if (!status) {
            return core::Status::failure(
                "inventory_ui.optimistic_replay_failed",
                "pending inventory transfer no longer applies: " + status.error().message);
        }
        ++stats.reapplied_optimistic_transfer_count;
    }
    return core::Status::ok();
}

core::Result<std::uint64_t>
InventoryUiViewModel::request_transfer(RuntimeSession& runtime,
                                       const world::InventoryTransferRequest& request,
                                       std::int64_t now_ms) {
    if (!displayed_inventory_.has_value() || request.source_owner_id != local_owner_id_ ||
        request.destination_owner_id != local_owner_id_) {
        return core::Result<std::uint64_t>::failure(
            "inventory_ui.invalid_transfer_owner",
            "inventory UI transfer must target the synchronized local inventory");
    }
    auto staged = *displayed_inventory_;
    auto status = world::transfer_inventory_items(staged, staged, request);
    if (!status) {
        return core::Result<std::uint64_t>::failure(status.error().code,
                                                    status.error().message);
    }
    auto submitted = runtime.submit_inventory_transfer(request, now_ms);
    if (!submitted) {
        return submitted;
    }
    displayed_inventory_ = std::move(staged);
    pending_.push_back({submitted.value(), request});
    total_mass_grams_ = calculate_total_mass_grams();
    return submitted;
}

std::uint64_t InventoryUiViewModel::calculate_total_mass_grams() const noexcept {
    if (!displayed_inventory_.has_value()) {
        return 0;
    }
    std::uint64_t total = 0;
    for (const auto& stack : displayed_inventory_->stacks) {
        const auto definition =
            std::ranges::find(item_definitions_, stack.prototype_id,
                              &items::ItemDefinition::prototype_id);
        if (definition == item_definitions_.end() ||
            (stack.count > 0 &&
             definition->mass_grams > std::numeric_limits<std::uint64_t>::max() / stack.count)) {
            continue;
        }
        const auto mass = definition->mass_grams * stack.count;
        total = mass > std::numeric_limits<std::uint64_t>::max() - total
                    ? std::numeric_limits<std::uint64_t>::max()
                    : total + mass;
    }
    return total;
}

core::SaveId InventoryUiViewModel::local_owner_id() const noexcept {
    return local_owner_id_;
}

const world::InventoryRecord* InventoryUiViewModel::displayed_inventory() const noexcept {
    return displayed_inventory_ ? &*displayed_inventory_ : nullptr;
}

std::span<const InventoryUiPendingTransfer>
InventoryUiViewModel::pending_transfers() const noexcept {
    return pending_;
}

const std::optional<core::Error>& InventoryUiViewModel::last_rejection() const noexcept {
    return last_rejection_;
}

std::uint64_t InventoryUiViewModel::total_mass_grams() const noexcept {
    return total_mass_grams_;
}

void InventoryUiViewModel::clear_rejection() noexcept {
    last_rejection_.reset();
}

GameUiLayer::GameUiLayer(std::span<const items::ItemDefinition> item_definitions,
                         std::span<const entities::EntityDefinition> entity_definitions,
                         ui::UiSkin skin)
    : item_definitions_(item_definitions), entity_definitions_(entity_definitions),
      inventory_(item_definitions), widgets_(std::move(skin)) {}

core::Status GameUiLayer::initialize() {
    if (initialized_) {
        return core::Status::failure("game_ui.already_initialized",
                                     "game UI layer is already initialized");
    }
    auto status = build_widgets();
    if (!status) {
        return status;
    }
    initialized_ = true;
    return core::Status::ok();
}

core::Status GameUiLayer::build_widgets() {
    const auto hud_root = ui::widget_id("game.hud.root");
    ui::WidgetDesc root;
    root.id = hud_root;
    root.layout.width = ui::UiSize::fill();
    root.layout.height = ui::UiSize::fill();
    root.layout.mode = ui::UiLayoutMode::overlay;
    root.layout.padding = {18.0F, 18.0F, 18.0F, 18.0F};
    root.color = {0.0F, 0.0F, 0.0F, 0.0F};
    auto status = widgets_.add(root);
    if (!status) {
        return status;
    }

    ui::WidgetDesc crosshair;
    crosshair.id = ui::widget_id("game.hud.crosshair");
    crosshair.parent = hud_root;
    crosshair.layout.width = ui::UiSize::pixels(3.0F);
    crosshair.layout.height = ui::UiSize::pixels(18.0F);
    crosshair.layout.horizontal_alignment = ui::UiAlignment::center;
    crosshair.layout.vertical_alignment = ui::UiAlignment::center;
    crosshair.color = {0.96F, 0.91F, 0.78F, 0.9F};
    status = widgets_.add(crosshair);
    if (!status) {
        return status;
    }

    auto vitals = make_panel(ui::widget_id("game.hud.vitals"), hud_root);
    vitals.layout.width = ui::UiSize::pixels(270.0F);
    vitals.layout.height = ui::UiSize::pixels(102.0F);
    vitals.layout.horizontal_alignment = ui::UiAlignment::start;
    vitals.layout.vertical_alignment = ui::UiAlignment::end;
    vitals.layout.mode = ui::UiLayoutMode::column;
    vitals.layout.padding = {12.0F, 10.0F, 12.0F, 10.0F};
    vitals.layout.gap = 5.0F;
    status = widgets_.add(vitals);
    if (!status) {
        return status;
    }
    constexpr std::array vital_names{"health", "stamina", "weight"};
    constexpr std::array vital_colors{
        std::array{0.42F, 0.08F, 0.06F, 0.96F},
        std::array{0.16F, 0.42F, 0.13F, 0.96F},
        std::array{0.44F, 0.30F, 0.10F, 0.96F},
    };
    for (std::size_t index = 0; index < vital_names.size(); ++index) {
        ui::WidgetDesc vital;
        vital.id = ui::widget_id("game.hud." + std::string(vital_names[index]));
        vital.parent = vitals.id;
        vital.kind = ui::WidgetKind::slider;
        vital.layout.width = ui::UiSize::fill();
        vital.layout.height = ui::UiSize::pixels(22.0F);
        vital.layout.padding = {6.0F, 4.0F, 6.0F, 4.0F};
        vital.color = vital_colors[index];
        vital.text = vital_names[index];
        vital.glyph_size_pixels = 11.0F;
        vital.maximum_value = 1.0F;
        vital.value = 1.0F;
        vital.enabled = false;
        status = widgets_.add(vital);
        if (!status) {
            return status;
        }
    }

    auto hotbar = make_panel(ui::widget_id("game.hud.hotbar"), hud_root);
    hotbar.layout.width = ui::UiSize::pixels(474.0F);
    hotbar.layout.height = ui::UiSize::pixels(58.0F);
    hotbar.layout.horizontal_alignment = ui::UiAlignment::center;
    hotbar.layout.vertical_alignment = ui::UiAlignment::end;
    hotbar.layout.mode = ui::UiLayoutMode::row;
    hotbar.layout.padding = {7.0F, 7.0F, 7.0F, 7.0F};
    hotbar.layout.gap = 4.0F;
    status = widgets_.add(hotbar);
    if (!status) {
        return status;
    }
    for (std::size_t index = 0; index < hotbar_slot_count; ++index) {
        ui::WidgetDesc slot;
        slot.id = ui::widget_id("game.hud.hotbar." + std::to_string(index));
        slot.parent = hotbar.id;
        slot.kind = ui::WidgetKind::grid_slot;
        slot.layout.width = ui::UiSize::pixels(46.0F);
        slot.layout.height = ui::UiSize::pixels(44.0F);
        slot.layout.padding = {4.0F, 4.0F, 4.0F, 4.0F};
        slot.nine_slice = "carved_slot";
        slot.color = index == 0 ? std::array{0.66F, 0.43F, 0.12F, 0.96F}
                                : std::array{0.18F, 0.09F, 0.035F, 0.94F};
        slot.glyph_size_pixels = 9.0F;
        status = widgets_.add(slot);
        if (!status) {
            return status;
        }
        hotbar_slots_.push_back(slot.id);
    }

    const auto inventory_root = ui::widget_id("game.inventory.root");
    ui::WidgetDesc modal_root;
    modal_root.id = inventory_root;
    modal_root.layout.width = ui::UiSize::fill();
    modal_root.layout.height = ui::UiSize::fill();
    modal_root.layout.mode = ui::UiLayoutMode::overlay;
    modal_root.color = {0.025F, 0.018F, 0.012F, 0.68F};
    modal_root.pointer_events = true;
    modal_root.blocks_gameplay = true;
    modal_root.visible = false;
    status = widgets_.add(modal_root);
    if (!status) {
        return status;
    }
    auto inventory_panel = make_panel(ui::widget_id("game.inventory.panel"), inventory_root);
    inventory_panel.layout.width = ui::UiSize::pixels(620.0F);
    inventory_panel.layout.height = ui::UiSize::pixels(470.0F);
    inventory_panel.layout.horizontal_alignment = ui::UiAlignment::center;
    inventory_panel.layout.vertical_alignment = ui::UiAlignment::center;
    inventory_panel.layout.mode = ui::UiLayoutMode::column;
    inventory_panel.layout.padding = {22.0F, 18.0F, 22.0F, 22.0F};
    inventory_panel.layout.gap = 12.0F;
    inventory_panel.color = {0.35F, 0.18F, 0.065F, 0.99F};
    status = widgets_.add(inventory_panel);
    if (!status) {
        return status;
    }
    ui::WidgetDesc title;
    title.id = ui::widget_id("game.inventory.title");
    title.parent = inventory_panel.id;
    title.kind = ui::WidgetKind::label;
    title.layout.width = ui::UiSize::fill();
    title.layout.height = ui::UiSize::pixels(28.0F);
    title.text = "Pack & Pockets";
    title.glyph_size_pixels = 18.0F;
    status = widgets_.add(title);
    if (!status) {
        return status;
    }
    auto grid = make_panel(ui::widget_id("game.inventory.grid"), inventory_panel.id);
    grid.layout.width = ui::UiSize::fill();
    grid.layout.height = ui::UiSize::fill();
    grid.layout.mode = ui::UiLayoutMode::grid;
    grid.layout.grid_columns = 9;
    grid.layout.grid_cell_height = 82.0F;
    grid.layout.padding = {8.0F, 8.0F, 8.0F, 8.0F};
    grid.layout.gap = 5.0F;
    grid.layout.clip_children = true;
    grid.color = {0.20F, 0.095F, 0.03F, 0.96F};
    status = widgets_.add(grid);
    if (!status) {
        return status;
    }
    for (std::size_t index = 0; index < inventory_slot_count; ++index) {
        ui::WidgetDesc slot;
        slot.id = ui::widget_id("game.inventory.slot." + std::to_string(index));
        slot.parent = grid.id;
        slot.kind = ui::WidgetKind::grid_slot;
        slot.layout.width = ui::UiSize::fill();
        slot.layout.height = ui::UiSize::fill();
        slot.layout.padding = {5.0F, 5.0F, 5.0F, 5.0F};
        slot.nine_slice = "carved_slot";
        slot.color = {0.13F, 0.06F, 0.022F, 0.98F};
        slot.glyph_size_pixels = 10.0F;
        slot.pointer_events = true;
        slot.focusable = true;
        slot.drop_target = true;
        status = widgets_.add(slot);
        if (!status) {
            return status;
        }
        inventory_slots_.push_back(slot.id);
    }
    return core::Status::ok();
}

core::Result<InventoryUiSynchronizationStats>
GameUiLayer::synchronize(const ClientRuntime& client) {
    if (!initialized_) {
        return core::Result<InventoryUiSynchronizationStats>::failure(
            "game_ui.not_initialized", "game UI layer must be initialized first");
    }
    auto synchronized = inventory_.synchronize(client);
    if (!synchronized) {
        return synchronized;
    }
    player_capacity_grams_ = player_capacity_grams(client);
    auto status = update_inventory_widgets();
    if (!status) {
        return core::Result<InventoryUiSynchronizationStats>::failure(status.error().code,
                                                                       status.error().message);
    }
    status = update_hud_widgets(client);
    if (!status) {
        return core::Result<InventoryUiSynchronizationStats>::failure(status.error().code,
                                                                       status.error().message);
    }
    stats_.inventory = synchronized.value();
    stats_.pending_inventory_transfers =
        static_cast<std::uint32_t>(inventory_.pending_transfers().size());
    stats_.inventory_mass_grams = inventory_.total_mass_grams();
    return synchronized;
}

core::Status GameUiLayer::update_inventory_widgets() {
    const auto* record = inventory_.displayed_inventory();
    if (record == nullptr) {
        return core::Status::failure("game_ui.inventory_unavailable",
                                     "local inventory view is not synchronized");
    }
    for (std::size_t index = 0; index < inventory_slots_.size(); ++index) {
        auto status = update_widget(widgets_, inventory_slots_[index],
                                    [this, record, index](ui::WidgetDesc& slot) {
            const auto occupied = index < record->stacks.size();
            slot.text = occupied ? item_label(record->stacks[index]) : std::string{};
            slot.tooltip =
                occupied ? item_tooltip(record->stacks[index], item_definitions_) : "Empty slot";
            slot.draggable = occupied;
            slot.drag_payload =
                occupied ? "inventory-slot:" + std::to_string(index) : std::string{};
            slot.color = occupied ? std::array{0.25F, 0.12F, 0.035F, 0.99F}
                                  : std::array{0.13F, 0.06F, 0.022F, 0.98F};
        });
        if (!status) {
            return status;
        }
    }
    for (std::size_t index = 0; index < hotbar_slots_.size(); ++index) {
        auto status = update_widget(widgets_, hotbar_slots_[index],
                                    [record, index](ui::WidgetDesc& slot) {
            slot.text = index < record->stacks.size() ? item_label(record->stacks[index])
                                                      : std::string{};
        });
        if (!status) {
            return status;
        }
    }
    return core::Status::ok();
}

core::Status GameUiLayer::update_hud_widgets(const ClientRuntime& client) {
    const auto* player = client.local_player_snapshot();
    if (player == nullptr) {
        return core::Status::failure("game_ui.player_snapshot_missing",
                                     "HUD requires a local replicated player snapshot");
    }
    const auto health =
        std::clamp(static_cast<float>(player->state.health_milli) /
                       static_cast<float>(hud_vital_pool_milli),
                   0.0F, 1.0F);
    const auto stamina =
        std::clamp(static_cast<float>(player->state.stamina_milli) /
                       static_cast<float>(hud_vital_pool_milli),
                   0.0F, 1.0F);
    const auto weight =
        std::clamp(static_cast<float>(inventory_.total_mass_grams()) /
                       static_cast<float>(std::max<std::uint64_t>(1, player_capacity_grams_)),
                   0.0F, 1.0F);
    auto status = update_widget(widgets_, ui::widget_id("game.hud.health"),
                                [health](ui::WidgetDesc& widget) {
        widget.value = health;
        widget.text =
            "Health " + std::to_string(static_cast<std::uint32_t>(health * 100.0F)) + "%";
    });
    if (!status) {
        return status;
    }
    status = update_widget(widgets_, ui::widget_id("game.hud.stamina"),
                           [stamina](ui::WidgetDesc& widget) {
        widget.value = stamina;
        widget.text =
            "Stamina " + std::to_string(static_cast<std::uint32_t>(stamina * 100.0F)) + "%";
    });
    if (!status) {
        return status;
    }
    return update_widget(widgets_, ui::widget_id("game.hud.weight"),
                         [this, weight](ui::WidgetDesc& widget) {
        widget.value = weight;
        widget.text = "Weight " + std::to_string(inventory_.total_mass_grams() / 1000) +
                      "/" + std::to_string(player_capacity_grams_ / 1000) + "kg";
    });
}

core::Result<GameUiProcessResult>
GameUiLayer::process_input(const platform::WindowInputSnapshot& input, RuntimeSession& runtime,
                           std::int64_t now_ms) {
    if (!initialized_) {
        return core::Result<GameUiProcessResult>::failure(
            "game_ui.not_initialized", "game UI layer must be initialized first");
    }
    GameUiProcessResult result;
    const auto toggle = key_pressed(input, platform::KeyCode::tab);
    const auto cancel = key_pressed(input, platform::KeyCode::escape);
    if (toggle || (cancel && inventory_open_)) {
        set_inventory_open(!inventory_open_);
        result.inventory_toggled = true;
        result.consumed.keyboard = true;
        result.consumed.blocks_gameplay = inventory_open_;
        return core::Result<GameUiProcessResult>::success(result);
    }
    auto ui_input = ui::UiInputFrame::from_platform(input);
    if (!inventory_open_ && cancel) {
        ui_input.navigation = ui::UiNavigation::none;
    }
    if (inventory_open_ && ui_input.navigation == ui::UiNavigation::next) {
        ui_input.navigation = ui::UiNavigation::none;
    }
    auto routed = widgets_.route_input(ui_input);
    result.consumed = routed.consumed;
    result.event_count = static_cast<std::uint32_t>(routed.events.size());
    auto command = handle_widget_events(
        routed.events, mouse_down(input, platform::MouseButton::right), runtime, now_ms);
    if (!command) {
        return core::Result<GameUiProcessResult>::failure(command.error().code,
                                                          command.error().message);
    }
    result.submitted_inventory_command = command.value();
    return core::Result<GameUiProcessResult>::success(result);
}

core::Result<std::optional<std::uint64_t>>
GameUiLayer::handle_widget_events(std::span<const ui::UiEvent> events, bool split_stack,
                                  RuntimeSession& runtime, std::int64_t now_ms) {
    for (const auto& event : events) {
        if (event.kind != ui::UiEventKind::dropped) {
            continue;
        }
        const auto source_slot = parse_slot_payload(event.payload);
        const auto target_slot = inventory_slot_from_widget(event.target);
        const auto* record = inventory_.displayed_inventory();
        if (!source_slot.has_value() || !target_slot.has_value() || record == nullptr ||
            *source_slot >= record->stacks.size()) {
            continue;
        }
        auto destination = *target_slot;
        if (destination >= record->stacks.size()) {
            destination = record->stacks.size();
        }
        auto count = record->stacks[*source_slot].count;
        if (split_stack && count > 1) {
            count = (count + 1U) / 2U;
        }
        world::InventoryTransferRequest request;
        request.source_owner_id = record->owner_id;
        request.destination_owner_id = record->owner_id;
        request.source_slot = *source_slot;
        request.destination_slot = destination;
        request.count = count;
        auto submitted = inventory_.request_transfer(runtime, request, now_ms);
        if (!submitted) {
            return core::Result<std::optional<std::uint64_t>>::failure(
                submitted.error().code, submitted.error().message);
        }
        auto status = update_inventory_widgets();
        if (!status) {
            return core::Result<std::optional<std::uint64_t>>::failure(status.error().code,
                                                                       status.error().message);
        }
        return core::Result<std::optional<std::uint64_t>>::success(submitted.value());
    }
    return core::Result<std::optional<std::uint64_t>>::success(std::nullopt);
}

std::optional<std::size_t>
GameUiLayer::inventory_slot_from_widget(ui::WidgetId id) const {
    const auto found = std::ranges::find(inventory_slots_, id);
    if (found == inventory_slots_.end()) {
        return std::nullopt;
    }
    return static_cast<std::size_t>(std::distance(inventory_slots_.begin(), found));
}

core::Result<ui::UiPaintStats>
GameUiLayer::paint(renderer::UiRenderer& renderer, renderer::rhi::RenderExtent extent,
                   float dpi_scale) {
    if (!initialized_ || !extent.is_valid()) {
        return core::Result<ui::UiPaintStats>::failure(
            "game_ui.invalid_paint_state",
            "game UI paint requires initialized widgets and a valid render extent");
    }
    const auto layout_start = std::chrono::steady_clock::now();
    auto status = widgets_.layout(
        {static_cast<float>(extent.width), static_cast<float>(extent.height)}, dpi_scale);
    stats_.layout_ms = std::chrono::duration<double, std::milli>(
                           std::chrono::steady_clock::now() - layout_start)
                           .count();
    if (!status) {
        return core::Result<ui::UiPaintStats>::failure(status.error().code,
                                                       status.error().message);
    }
    const auto paint_start = std::chrono::steady_clock::now();
    auto painted = widgets_.paint(renderer);
    stats_.paint_ms = std::chrono::duration<double, std::milli>(
                          std::chrono::steady_clock::now() - paint_start)
                          .count();
    if (painted) {
        stats_.layout = widgets_.layout_stats();
        stats_.paint = painted.value();
        stats_.pending_inventory_transfers =
            static_cast<std::uint32_t>(inventory_.pending_transfers().size());
        stats_.inventory_mass_grams = inventory_.total_mass_grams();
        stats_.inventory_open = inventory_open_;
    }
    return painted;
}

std::uint64_t GameUiLayer::player_capacity_grams(const ClientRuntime& client) const noexcept {
    const auto local_player = client.local_player_net_id();
    const auto* entity = local_player.is_valid()
                             ? client.world().entities().find_by_net_id(local_player)
                             : nullptr;
    const auto player_prototype = core::PrototypeId::parse("base:entities/player");
    const auto prototype =
        entity != nullptr
            ? std::optional<core::PrototypeId>{entity->prototype_id}
            : player_prototype;
    if (!prototype.has_value()) {
        return 1;
    }
    const auto definition = std::ranges::find(
        entity_definitions_, *prototype, &entities::EntityDefinition::prototype_id);
    return definition == entity_definitions_.end()
               ? 1
               : std::max<std::uint64_t>(1, definition->carry_capacity_grams);
}

void GameUiLayer::set_inventory_open(bool open) noexcept {
    inventory_open_ = open;
    widgets_.set_visible(ui::widget_id("game.inventory.root"), open);
    if (open && !inventory_slots_.empty()) {
        widgets_.set_focus(inventory_slots_.front());
    } else if (!open) {
        widgets_.set_focus({});
    }
    stats_.inventory_open = open;
}

bool GameUiLayer::inventory_open() const noexcept {
    return inventory_open_;
}

bool GameUiLayer::blocks_gameplay() const noexcept {
    return inventory_open_;
}

InventoryUiViewModel& GameUiLayer::inventory() noexcept {
    return inventory_;
}

const InventoryUiViewModel& GameUiLayer::inventory() const noexcept {
    return inventory_;
}

ui::WidgetTree& GameUiLayer::widgets() noexcept {
    return widgets_;
}

const ui::WidgetTree& GameUiLayer::widgets() const noexcept {
    return widgets_;
}

const GameUiStats& GameUiLayer::stats() const noexcept {
    return stats_;
}

} // namespace heartstead::game
