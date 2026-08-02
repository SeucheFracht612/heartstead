#include "engine/server/admin_service.hpp"
#include "engine/server/authoritative_server.hpp"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

namespace {

[[nodiscard]] std::filesystem::path temporary_root() {
    return std::filesystem::temp_directory_path() /
           ("heartstead_authoritative_server_v02_" +
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
}

void flush_outbound(heartstead::server::AuthoritativeServer& server) {
    heartstead::net::HostSessionTickResult tick;
    assert(server.host_session().flush_outbound(tick));
    assert(heartstead::net::validate_host_session_outbound_delivery_report(tick.outbound_delivery));
}

void test_join_profile_discovery_chat_and_leave_lifecycle() {
    const auto root = temporary_root();
    const auto uuid =
        heartstead::player_profiles::PlayerUuid::parse("123e4567-e89b-12d3-a456-426614174000");
    assert(uuid);

    heartstead::server::AuthoritativeServerConfig config;
    config.world_root = root / "world";
    config.server_root = root / "server";
    config.server_session = "test_session";
    config.expected_content_fingerprint = "base-hash";
    heartstead::server::AuthoritativeServer server(config);
    assert(server.start());

    heartstead::server::PlayerJoinRequest request{*uuid, "Settler", "203.0.113.42:7777",
                                                  "wrong-hash"};
    auto mismatch = server.join(request, 100);
    assert(!mismatch);
    assert(mismatch.error().code == "authoritative_server.content_mismatch");
    assert(server.connected_player_count() == 0);

    request.client_content_fingerprint = "base-hash";
    auto joined = server.join(request, 101);
    assert(joined);
    assert(server.connected_player_count() == 1);
    flush_outbound(server);
    auto initial_messages = server.host_session().drain_client_messages(joined.value());
    assert(initial_messages);
    const auto has_initial_type = [&initial_messages](std::string_view type) {
        return std::ranges::any_of(initial_messages.value(), [type](const auto& envelope) {
            return envelope.message.payload_type == type;
        });
    };
    assert(has_initial_type("player_profile"));
    assert(has_initial_type("map_discovery"));

    auto discovered = server.discover(joined.value(), "caves", {-65, 130}, 102);
    assert(discovered && discovered.value());
    auto duplicate = server.discover(joined.value(), "caves", {-65, 130}, 103);
    assert(duplicate && !duplicate.value());
    flush_outbound(server);
    auto map_delta = server.host_session().drain_client_messages(joined.value());
    assert(map_delta);
    assert(std::ranges::any_of(map_delta.value(), [](const auto& envelope) {
        return envelope.message.payload_type == "map_discovery_delta";
    }));

    assert(server.chat(joined.value(), "local", "hello settlement", 104));
    flush_outbound(server);
    auto chat_message = server.host_session().drain_client_messages(joined.value());
    assert(chat_message);
    assert(std::ranges::any_of(chat_message.value(), [](const auto& envelope) {
        return envelope.message.payload_type == "chat";
    }));
    auto chat_log = server.logs().query_current(heartstead::server_logs::ServerLogCategory::chat);
    assert(chat_log && chat_log.value().size() == 1);
    assert(chat_log.value().front().message == "hello settlement");

    assert(server.flush_dirty_profiles());
    assert(server.leave(joined.value(), 105));
    assert(server.connected_player_count() == 0);
    auto stored = server.profile_store().load(*uuid);
    assert(stored);
    assert(stored.value().map_discovery.is_discovered("caves", {-65, 130}));

    auto rejoined = server.join(request, 106);
    assert(rejoined);
    flush_outbound(server);
    auto restored_messages = server.host_session().drain_client_messages(rejoined.value());
    assert(restored_messages);
    const auto restored_map_message =
        std::ranges::find_if(restored_messages.value(), [](const auto& envelope) {
            return envelope.message.payload_type == "map_discovery";
        });
    assert(restored_map_message != restored_messages.value().end());
    auto restored_map = heartstead::player_profiles::MapDiscoveryTextCodec::decode(
        restored_map_message->message.payload);
    assert(restored_map);
    assert(restored_map.value().is_discovered("caves", {-65, 130}));
    assert(server.stop(107));

    heartstead::server_logs::ServerLogFilter join_filter;
    join_filter.event_type = "join";
    auto joins = server.logs().query_current(heartstead::server_logs::ServerLogCategory::general,
                                             join_filter);
    assert(joins && joins.value().size() == 2);
    assert(joins.value().front().metadata.at("address_hash").starts_with("stable64:"));
    assert(!joins.value().front().metadata.at("address_hash").contains("203.0.113.42"));

    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
}

void test_admin_backups_publish_complete_unique_directories_outside_the_world() {
    const auto root = temporary_root();
    const auto world_root = root / "world";
    const auto server_root = root / "server";
    const auto backup_root = root / "backups";
    std::filesystem::create_directories(world_root);
    {
        std::ofstream state(world_root / "state.txt", std::ios::trunc);
        state << "authoritative-state";
        state.close();
        assert(state);
    }

    heartstead::server::AuthoritativeServerConfig server_config;
    server_config.world_root = world_root;
    server_config.server_root = server_root;
    heartstead::server::AuthoritativeServer server(server_config);
    assert(server.start());

    {
        heartstead::server::AdminServiceConfig admin_config;
        admin_config.world_root = world_root;
        admin_config.server_root = server_root;
        admin_config.backup_root = backup_root;
        heartstead::server::ServerAdminService admin(admin_config, server);
        assert(admin.load_bans());

        auto first = admin.create_backup("manual", 10, "console");
        auto second = admin.create_backup("manual", 11, "console");
        assert(first && second && first.value() != second.value());
        assert(std::filesystem::is_regular_file(first.value() / "state.txt"));
        assert(std::filesystem::is_regular_file(second.value() / "state.txt"));
        assert(!std::filesystem::exists(first.value().string() + ".tmp"));
        assert(!std::filesystem::exists(second.value().string() + ".tmp"));
    }

    {
        heartstead::server::AdminServiceConfig unsafe_config;
        unsafe_config.world_root = world_root;
        unsafe_config.server_root = server_root;
        unsafe_config.backup_root = world_root / "nested_backups";
        heartstead::server::ServerAdminService unsafe_admin(unsafe_config, server);
        assert(unsafe_admin.load_bans());
        auto rejected = unsafe_admin.create_backup("unsafe", 12, "console");
        assert(!rejected);
        assert(rejected.error().code == "admin.backup_inside_world");
    }

    assert(server.stop(13));
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
}

void test_server_start_preflights_log_storage_and_session_identity() {
    const auto root = temporary_root();
    heartstead::server::AuthoritativeServerConfig invalid_session_config;
    invalid_session_config.world_root = root / "world";
    invalid_session_config.server_root = root / "server";
    invalid_session_config.server_session.assign(129, 's');
    heartstead::server::AuthoritativeServer invalid_session_server(invalid_session_config);
    auto invalid_session = invalid_session_server.start();
    assert(!invalid_session);
    assert(invalid_session.error().code == "authoritative_server.invalid_config");
    assert(!invalid_session_server.is_running());

    const auto unsafe_server_root = root / "unsafe_server";
    std::filesystem::create_directories(unsafe_server_root / "logs" / "audit");
    const auto outside = root / "outside.log";
    {
        std::ofstream output(outside);
        output << "outside";
    }
    std::error_code link_error;
    std::filesystem::create_symlink(outside, unsafe_server_root / "logs" / "audit" / "current.log",
                                    link_error);
    if (!link_error) {
        heartstead::server::AuthoritativeServerConfig unsafe_config;
        unsafe_config.world_root = root / "unsafe_world";
        unsafe_config.server_root = unsafe_server_root;
        heartstead::server::AuthoritativeServer unsafe_server(unsafe_config);
        auto unsafe_start = unsafe_server.start();
        assert(!unsafe_start);
        assert(unsafe_start.error().code == "server_log.unsafe_symlink");
        assert(!unsafe_server.is_running());
    }

    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
}

} // namespace

int main() {
    test_join_profile_discovery_chat_and_leave_lifecycle();
    test_admin_backups_publish_complete_unique_directories_outside_the_world();
    test_server_start_preflights_log_storage_and_session_identity();
    return 0;
}
