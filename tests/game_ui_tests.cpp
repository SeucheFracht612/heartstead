#include "engine/content/content_validation.hpp"
#include "engine/input/input_action.hpp"
#include "game/testing/headless_session.hpp"
#include "game/ui/game_ui.hpp"

#include <cassert>
#include <filesystem>
#include <ranges>
#include <utility>

namespace {

using namespace heartstead;

std::filesystem::path source_root() {
    return std::filesystem::path(HEARTSTEAD_TEST_SOURCE_DIR);
}

struct UiRuntimeFixture {
    content::ContentValidationReport content;
    std::unique_ptr<game::HeadlessSessionHarness> harness;
};

[[nodiscard]] UiRuntimeFixture make_fixture() {
    auto content = content::ContentValidation::validate(source_root());
    assert(!content.has_errors());
    game::HeadlessSessionDesc desc;
    desc.source_root = source_root();
    auto harness = game::HeadlessSessionHarness::create(std::move(desc));
    assert(harness);
    return {std::move(content), std::move(harness).value()};
}

[[nodiscard]] core::SaveId local_player_save_id(const game::ClientRuntime& client) {
    const auto* player = client.local_player_snapshot();
    assert(player != nullptr);
    return player->player_save_id;
}

void test_optimistic_inventory_transfer_confirms_authoritatively() {
    auto fixture = make_fixture();
    auto* session = fixture.harness->runtime().session();
    assert(session != nullptr && session->client() != nullptr);
    game::InventoryUiViewModel model(fixture.content.item_definitions);
    auto synchronized = model.synchronize(*session->client());
    assert(synchronized);
    const auto* initial = model.displayed_inventory();
    assert(initial != nullptr);
    assert(initial->stacks.size() == 2);
    const auto first_item = initial->stacks.front().prototype_id;
    const auto owner = local_player_save_id(*session->client());

    world::InventoryTransferRequest request;
    request.source_owner_id = owner;
    request.destination_owner_id = owner;
    request.source_slot = 0;
    request.destination_slot = initial->stacks.size();
    request.count = initial->stacks.front().count;
    auto submitted = model.request_transfer(*session, request, 10);
    assert(submitted);
    assert(model.pending_transfers().size() == 1);
    assert(model.displayed_inventory()->stacks.back().prototype_id == first_item);

    auto tick = fixture.harness->run_ticks(1);
    assert(tick);
    synchronized = model.synchronize(*session->client());
    assert(synchronized);
    assert(synchronized.value().confirmed_transfer_count == 1);
    assert(model.pending_transfers().empty());
    assert(model.displayed_inventory()->stacks.back().prototype_id == first_item);
    const auto* authoritative = session->client()->world().inventories().find(owner);
    assert(authoritative != nullptr);
    assert(authoritative->stacks == model.displayed_inventory()->stacks);
    assert(fixture.harness->shutdown());
}

void test_rejected_transfer_rolls_back_to_replicated_inventory() {
    auto fixture = make_fixture();
    auto* session = fixture.harness->runtime().session();
    assert(session != nullptr && session->client() != nullptr && session->server() != nullptr);
    game::InventoryUiViewModel model(fixture.content.item_definitions);
    assert(model.synchronize(*session->client()));
    const auto before = *model.displayed_inventory();
    const auto owner = before.owner_id;

    world::InventoryTransferRequest request;
    request.source_owner_id = owner;
    request.destination_owner_id = owner;
    request.source_slot = 0;
    request.destination_slot = before.stacks.size();
    request.count = before.stacks.front().count;
    auto submitted = model.request_transfer(*session, request, 20);
    assert(submitted);
    assert(model.displayed_inventory()->stacks != before.stacks);

    auto* server_inventory = session->server()->world().inventories().find(owner);
    assert(server_inventory != nullptr);
    server_inventory->stacks.clear();
    auto tick = fixture.harness->run_ticks(1);
    assert(tick);
    auto synchronized = model.synchronize(*session->client());
    assert(synchronized);
    assert(synchronized.value().rejected_transfer_count == 1);
    assert(synchronized.value().rolled_back_transfer_count == 1);
    assert(model.pending_transfers().empty());
    assert(model.last_rejection().has_value());
    assert(model.last_rejection()->code == "inventory_transfer.source_slot_out_of_range");
    assert(model.displayed_inventory()->stacks == before.stacks);
    assert(fixture.harness->shutdown());
}

void test_inventory_modal_consumes_input_before_gameplay() {
    auto fixture = make_fixture();
    auto* session = fixture.harness->runtime().session();
    assert(session != nullptr && session->client() != nullptr);
    game::GameUiLayer ui_layer(fixture.content.item_definitions,
                               fixture.content.entity_definitions, fixture.content.ui_skin);
    assert(ui_layer.initialize());
    assert(ui_layer.synchronize(*session->client()));

    platform::WindowInputSnapshot tab;
    tab.pressed_keys = {platform::KeyCode::tab};
    tab.down_keys = {platform::KeyCode::tab};
    auto processed = ui_layer.process_input(tab, *session, 30);
    assert(processed);
    assert(processed.value().inventory_toggled);
    assert(processed.value().consumed.keyboard);
    assert(processed.value().consumed.blocks_gameplay);
    assert(ui_layer.inventory_open());

    auto actions = input::InputActionMap::gameplay_defaults();
    platform::WindowInputSnapshot blocked;
    blocked.pressed_keys = {platform::KeyCode::w};
    blocked.down_keys = {platform::KeyCode::w};
    processed = ui_layer.process_input(blocked, *session, 31);
    assert(processed);
    assert(processed.value().consumed.blocks_gameplay);
    assert(actions.evaluate(blocked)[input::InputAction::move_forward].held);
    assert(ui_layer.blocks_gameplay());

    platform::WindowInputSnapshot escape;
    escape.pressed_keys = {platform::KeyCode::escape};
    processed = ui_layer.process_input(escape, *session, 32);
    assert(processed);
    assert(!ui_layer.inventory_open());
    assert(!ui_layer.blocks_gameplay());
    assert(fixture.harness->shutdown());
}

void test_hud_is_bound_to_replicated_vitals_and_inventory_mass() {
    auto fixture = make_fixture();
    auto* client = fixture.harness->runtime().session()->client();
    assert(client != nullptr);
    game::GameUiLayer ui_layer(fixture.content.item_definitions,
                               fixture.content.entity_definitions, fixture.content.ui_skin);
    assert(ui_layer.initialize());
    assert(ui_layer.synchronize(*client));
    const auto* player = client->local_player_snapshot();
    assert(player != nullptr);
    assert(ui_layer.stats().inventory_mass_grams == 1008);
    const auto* health = ui_layer.widgets().find(ui::widget_id("game.hud.health"));
    const auto* stamina = ui_layer.widgets().find(ui::widget_id("game.hud.stamina"));
    const auto* weight = ui_layer.widgets().find(ui::widget_id("game.hud.weight"));
    assert(health != nullptr && stamina != nullptr && weight != nullptr);
    assert(health->value ==
           static_cast<float>(player->state.health_milli) / 100'000.0F);
    assert(stamina->value ==
           static_cast<float>(player->state.stamina_milli) / 100'000.0F);
    assert(weight->value > 0.0F);
    assert(fixture.harness->shutdown());
}

void test_hotbar_selection_tracks_keyboard_and_wheel() {
    auto fixture = make_fixture();
    auto* session = fixture.harness->runtime().session();
    assert(session != nullptr && session->client() != nullptr);
    game::GameUiLayer ui_layer(fixture.content.item_definitions,
                               fixture.content.entity_definitions, fixture.content.ui_skin);
    assert(ui_layer.initialize());
    assert(ui_layer.synchronize(*session->client()));
    assert(ui_layer.selected_hotbar_slot() == 0);
    assert(ui_layer.selected_hotbar_item() != nullptr);

    platform::WindowInputSnapshot number;
    number.pressed_keys = {platform::KeyCode::digit_2};
    auto processed = ui_layer.process_input(number, *session, 40);
    assert(processed && processed.value().hotbar_selection_changed);
    assert(ui_layer.selected_hotbar_slot() == 1);
    assert(ui_layer.selected_hotbar_item() != nullptr);

    platform::WindowInputSnapshot wheel;
    wheel.wheel_delta_y = -1;
    processed = ui_layer.process_input(wheel, *session, 41);
    assert(processed && processed.value().hotbar_selection_changed);
    assert(ui_layer.selected_hotbar_slot() == 2);
    assert(ui_layer.selected_hotbar_item() == nullptr);
    assert(!ui_layer.set_selected_hotbar_slot(9));
    assert(fixture.harness->shutdown());
}

} // namespace

int main() {
    test_optimistic_inventory_transfer_confirms_authoritatively();
    test_rejected_transfer_rolls_back_to_replicated_inventory();
    test_inventory_modal_consumes_input_before_gameplay();
    test_hud_is_bound_to_replicated_vitals_and_inventory_mass();
    test_hotbar_selection_tracks_keyboard_and_wheel();
    return 0;
}
