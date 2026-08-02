#include "engine/net/replication_budget.hpp"

#include <cassert>

using namespace heartstead;

namespace {

net::ReplicationTickBudgetConfig test_config() {
    net::ReplicationTickBudgetConfig config;
    config.max_messages_per_tick = 3;
    config.max_payload_bytes_per_tick = 30;
    config.max_serialization_time_us_per_tick = 100;
    config.max_messages_per_client_per_tick = 2;
    config.max_payload_bytes_per_client_per_tick = 20;
    config.max_serialization_time_us_per_client_per_tick = 50;
    return config;
}

void finish_serialization(net::ReplicationTickBudget& budget, std::uint64_t elapsed_us) {
    const auto began = budget.begin_shared_serialization();
    assert(began && began.value());
    assert(budget.finish_shared_serialization(elapsed_us));
}

void test_config_rejects_every_zero_limit() {
    auto config = test_config();
    assert(config.validate());

    config.max_messages_per_tick = 0;
    assert(!config.validate());
    config = test_config();
    config.max_payload_bytes_per_tick = 0;
    assert(!config.validate());
    config = test_config();
    config.max_serialization_time_us_per_tick = 0;
    assert(!config.validate());
    config = test_config();
    config.max_messages_per_client_per_tick = 0;
    assert(!config.validate());
    config = test_config();
    config.max_payload_bytes_per_client_per_tick = 0;
    assert(!config.validate());
    config = test_config();
    config.max_serialization_time_us_per_client_per_tick = 0;
    assert(!config.validate());
}

void test_global_and_per_client_admission_is_strict_and_deterministic() {
    auto config = test_config();
    config.max_payload_bytes_per_tick = 40;
    net::ReplicationTickBudget budget(config);
    assert(budget.begin_tick(7));
    const auto client_one = core::NetId::from_value(1);
    const auto client_two = core::NetId::from_value(2);

    finish_serialization(budget, 10);
    auto admission = budget.admit_prepared(client_two, 10, 10);
    assert(admission && admission.value());
    admission = budget.admit_prepared(client_one, 10, 10);
    assert(admission && admission.value());

    admission = budget.admit_prepared(client_two, 11, 10);
    assert(admission && !admission.value());
    assert(admission.value().limit == net::ReplicationBudgetLimit::client_payload_bytes);

    admission = budget.admit_prepared(client_one, 10, 10);
    assert(admission && admission.value());
    admission = budget.admit_prepared(client_two, 1, 1);
    assert(admission && !admission.value());
    assert(admission.value().limit == net::ReplicationBudgetLimit::global_message_count);

    const auto stats = budget.snapshot();
    assert(stats.tick == 7);
    assert(stats.considered_message_count == 5);
    assert(stats.admitted_message_count == 3);
    assert(stats.deferred_message_count == 2);
    assert(stats.admitted_payload_bytes == 30);
    assert(stats.attributed_serialization_time_us == 41);
    assert(stats.shared_serialization_operation_count == 1);
    assert(stats.shared_serialization_time_us == 10);
    assert(stats.clients.size() == 2);
    assert(stats.clients[0].client_id == client_one);
    assert(stats.clients[1].client_id == client_two);
    assert(stats.clients[0].admitted_message_count == 2);
    assert(stats.clients[1].admitted_message_count == 1);
    assert(stats.deferrals.client_payload_bytes == 1);
    assert(stats.deferrals.global_message_count == 1);
    assert(net::validate_replication_tick_budget_stats(stats));
}

void test_shared_serialization_has_one_operation_overshoot() {
    auto config = test_config();
    config.max_messages_per_tick = 10;
    config.max_payload_bytes_per_tick = 1'000;
    config.max_serialization_time_us_per_tick = 100;
    config.max_messages_per_client_per_tick = 10;
    config.max_payload_bytes_per_client_per_tick = 1'000;
    config.max_serialization_time_us_per_client_per_tick = 1'000;
    net::ReplicationTickBudget budget(config);
    assert(budget.begin_tick(9));
    const auto client = core::NetId::from_value(3);

    finish_serialization(budget, 75);
    auto admission = budget.admit_prepared(client, 8, 75);
    assert(admission && admission.value());
    finish_serialization(budget, 40);
    admission = budget.admit_prepared(client, 8, 40);
    assert(admission && admission.value());

    const auto began = budget.begin_shared_serialization();
    assert(began && !began.value());
    assert(budget.record_deferred(client, net::ReplicationBudgetLimit::global_serialization_time));

    const auto stats = budget.snapshot();
    assert(stats.shared_serialization_time_us == 115);
    assert(stats.maximum_shared_serialization_time_us == 75);
    assert(stats.serialization_time_overshoot_us == 15);
    assert(stats.global_serialization_budget_exhausted);
    assert(stats.deferrals.global_serialization_time == 1);
    assert(net::validate_replication_tick_budget_stats(stats));
}

void test_per_client_time_is_attributed_without_double_counting_global_time() {
    auto config = test_config();
    config.max_messages_per_tick = 10;
    config.max_payload_bytes_per_tick = 1'000;
    config.max_messages_per_client_per_tick = 10;
    config.max_payload_bytes_per_client_per_tick = 1'000;
    config.max_serialization_time_us_per_client_per_tick = 20;
    net::ReplicationTickBudget budget(config);
    assert(budget.begin_tick(11));
    const auto client_one = core::NetId::from_value(10);
    const auto client_two = core::NetId::from_value(20);

    finish_serialization(budget, 15);
    auto admission = budget.admit_prepared(client_one, 4, 15);
    assert(admission && admission.value());
    admission = budget.admit_prepared(client_two, 4, 15);
    assert(admission && admission.value());

    finish_serialization(budget, 10);
    admission = budget.admit_prepared(client_one, 4, 10);
    assert(admission && admission.value());
    admission = budget.admit_prepared(client_two, 4, 10);
    assert(admission && admission.value());

    auto limit = budget.preparation_limit(client_one);
    assert(limit && limit.value() == net::ReplicationBudgetLimit::client_serialization_time);
    assert(budget.record_deferred(client_one, limit.value()));
    limit = budget.preparation_limit(client_two);
    assert(limit && limit.value() == net::ReplicationBudgetLimit::client_serialization_time);
    assert(budget.record_deferred(client_two, limit.value()));

    const auto stats = budget.snapshot();
    assert(stats.shared_serialization_time_us == 25);
    assert(stats.attributed_serialization_time_us == 50);
    assert(stats.deferrals.client_serialization_time == 2);
    assert(stats.clients[0].serialization_time_overshoot_us == 5);
    assert(stats.clients[1].serialization_time_overshoot_us == 5);
    assert(net::validate_replication_tick_budget_stats(stats));
}

void test_preflight_and_api_misuse_fail_closed() {
    net::ReplicationTickBudget budget(test_config());
    assert(budget.begin_tick(1));
    const auto client = core::NetId::from_value(5);

    assert(!budget.preparation_limit({}));
    assert(!budget.record_deferred({}, net::ReplicationBudgetLimit::global_message_count));
    assert(!budget.record_deferred(client, net::ReplicationBudgetLimit::none));
    assert(!budget.record_deferred(client, static_cast<net::ReplicationBudgetLimit>(255)));
    assert(!budget.finish_shared_serialization(1));

    auto began = budget.begin_shared_serialization();
    assert(began && began.value());
    assert(!budget.begin_shared_serialization());
    assert(!budget.begin_tick(2));
    assert(budget.finish_shared_serialization(100));

    const auto limit = budget.preparation_limit(client);
    assert(limit && limit.value() == net::ReplicationBudgetLimit::global_serialization_time);
    began = budget.begin_shared_serialization();
    assert(began && !began.value());
    assert(net::validate_replication_tick_budget_stats(budget.snapshot()));
}

} // namespace

int main() {
    test_config_rejects_every_zero_limit();
    test_global_and_per_client_admission_is_strict_and_deterministic();
    test_shared_serialization_has_one_operation_overshoot();
    test_per_client_time_is_attributed_without_double_counting_global_time();
    test_preflight_and_api_misuse_fail_closed();
    return 0;
}
