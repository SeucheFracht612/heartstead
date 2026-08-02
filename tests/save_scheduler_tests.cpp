#include "engine/save/save_scheduler.hpp"
#include "engine/save/save_slot.hpp"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

[[nodiscard]] std::filesystem::path make_temp_root() {
    const auto parent = std::filesystem::temp_directory_path();
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    for (std::uint32_t attempt = 0; attempt < 100; ++attempt) {
        const auto root = parent / ("heartstead_save_scheduler_" + std::to_string(nonce) + "_" +
                                    std::to_string(attempt));
        std::error_code error;
        if (std::filesystem::create_directory(root, error)) {
            return root;
        }
    }
    assert(false && "could not create save scheduler test directory");
    return {};
}

[[nodiscard]] heartstead::save::SaveSnapshot snapshot(std::uint64_t seed,
                                                      std::size_t delta_bytes = 16) {
    heartstead::save::SaveSnapshot result;
    result.metadata.game_version = "0.1.0";
    result.metadata.world_seed = seed;
    result.metadata.enabled_mods.push_back({"base", "0.0.1", "hash"});
    result.chunk_edits.push_back({{1, 2, 3}, std::string(delta_bytes, 'd')});
    result.mod_states.push_back({"base", "save_scheduler_test", std::to_string(seed)});
    return result;
}

[[nodiscard]] std::vector<heartstead::save::SaveResult>
wait_for_results(heartstead::save::SaveScheduler& scheduler, std::size_t expected) {
    std::vector<heartstead::save::SaveResult> results;
    for (std::size_t attempt = 0; attempt < 10'000 && results.size() < expected; ++attempt) {
        auto completed = scheduler.drain_completed();
        for (auto& result : completed) {
            results.push_back(std::move(result));
        }
        if (results.size() < expected) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    assert(results.size() == expected);
    return results;
}

void cleanup(const std::filesystem::path& root) {
    std::error_code error;
    std::filesystem::remove_all(root, error);
    assert(!error);
}

void test_memory_estimate_accounts_for_owned_payloads() {
    auto small = snapshot(1, 16);
    auto large = snapshot(2, 512U * 1024U);
    large.build_pieces.push_back({});
    large.build_pieces.back().material_tags = {"stone", "wood", "clay"};

    const auto small_memory = heartstead::save::estimate_save_snapshot_memory(small);
    const auto large_memory = heartstead::save::estimate_save_snapshot_memory(large);
    assert(!small_memory.saturated);
    assert(!large_memory.saturated);
    assert(small_memory.retained_bytes >= sizeof(heartstead::save::SaveSnapshot));
    assert(small_memory.working_reservation_bytes > small_memory.retained_bytes);
    assert(large_memory.retained_bytes > small_memory.retained_bytes + 500U * 1024U);
    assert(large_memory.working_reservation_bytes > small_memory.working_reservation_bytes);
}

void test_background_save_is_bounded_durable_and_compacted() {
    const auto root = make_temp_root();
    heartstead::save::SaveSchedulerConfig config;
    config.max_concurrent_requests = 1;
    config.max_completed_results = 1;
    auto created = heartstead::save::SaveScheduler::create(config);
    assert(created);
    auto scheduler = std::move(created).value();

    const auto started = std::chrono::steady_clock::now();
    auto submitted = scheduler->submit({root, snapshot(41), true});
    const auto submission_elapsed = std::chrono::steady_clock::now() - started;
    assert(submitted);
    // This is a unit-level guard against accidental inline disk I/O. The release benchmark owns
    // the strict 0.25 ms acceptance gate.
    assert(submission_elapsed < std::chrono::milliseconds(50));
    assert(scheduler->has_in_flight());
    assert(scheduler->stats().reserved_working_bytes > 0);
    assert(scheduler->stats().reserved_working_bytes_high_water ==
           scheduler->stats().reserved_working_bytes);

    // Active requests remain counted until owner-thread publication, even if the worker finishes
    // between these calls, so completed mailboxes cannot open an unbounded submission window.
    auto rejected = scheduler->submit({root, snapshot(42), true});
    assert(!rejected);
    assert(rejected.error().code == "save_scheduler.full");

    auto results = wait_for_results(*scheduler, 1);
    assert(results.front().request_id == submitted.value());
    assert(results.front().state == heartstead::save::SaveResultState::succeeded);
    assert(results.front().durably_accepted);
    assert(results.front().compacted);
    assert(results.front().journal_sequence == 1);
    assert(results.front().encoded_bytes > 0);
    assert(results.front().reserved_working_bytes > 0);
    assert(results.front().durable_acceptance_ms > 0.0);
    assert(results.front().compaction_ms > 0.0);
    assert(results.front().total_worker_ms >= results.front().durable_acceptance_ms);
    assert(!scheduler->has_in_flight());
    assert(scheduler->stats().reserved_working_bytes == 0);
    assert(scheduler->stats().submitted_requests == 1);
    assert(scheduler->stats().rejected_requests == 1);
    assert(scheduler->stats().durably_accepted_requests == 1);
    assert(scheduler->stats().compacted_requests == 1);

    auto loaded = heartstead::save::FileSaveDatabase(root).read_snapshot();
    assert(loaded && loaded.value().metadata.world_seed == 41);
    cleanup(root);
}

void test_durable_only_result_recovers_without_compaction() {
    const auto root = make_temp_root();
    auto created = heartstead::save::SaveScheduler::create();
    assert(created);
    auto scheduler = std::move(created).value();
    auto submitted = scheduler->submit({root, snapshot(51), false});
    assert(submitted);
    auto results = wait_for_results(*scheduler, 1);
    assert(results.front().durably_accepted);
    assert(!results.front().compacted);
    assert(results.front().compaction_ms == 0.0);
    assert(!std::filesystem::exists(root / "current.txt"));
    auto loaded = heartstead::save::FileSaveDatabase(root).read_snapshot();
    assert(loaded && loaded.value().metadata.world_seed == 51);
    cleanup(root);
}

void test_slot_metadata_is_committed_on_the_worker() {
    const auto root = make_temp_root();
    const auto catalog_root = root / "saves";
    heartstead::save::FileSaveSlotCatalog catalog(catalog_root);
    assert(catalog.create_slot("worker-slot"));

    auto created = heartstead::save::SaveScheduler::create();
    assert(created);
    auto scheduler = std::move(created).value();
    heartstead::save::SaveRequest request;
    request.database_root = catalog_root / "worker-slot";
    request.snapshot = snapshot(52);
    request.compact_after_acceptance = false;
    request.slot_metadata_update = heartstead::save::SaveRequest::SlotMetadataUpdate{
        catalog_root, "worker-slot", 12'345};
    auto submitted = scheduler->submit(std::move(request));
    assert(submitted);

    auto results = scheduler->wait_for_completed(std::chrono::seconds(10));
    assert(results.size() == 1);
    assert(results.front().durably_accepted);
    assert(results.front().slot_metadata_updated);
    assert(results.front().metadata_error_code.empty());
    auto metadata = catalog.read_metadata("worker-slot");
    assert(metadata);
    assert(metadata.value().created_at_ms == 12'345);
    assert(metadata.value().last_saved_at_ms == 12'345);

    heartstead::save::SaveRequest mismatched;
    mismatched.database_root = catalog_root / "worker-slot";
    mismatched.snapshot = snapshot(53);
    mismatched.slot_metadata_update = heartstead::save::SaveRequest::SlotMetadataUpdate{
        catalog_root, "different-slot", 12'346};
    auto rejected = scheduler->submit(std::move(mismatched));
    assert(!rejected);
    assert(rejected.error().code == "save_scheduler.invalid_request");
    cleanup(root);
}

void test_queued_request_cancellation_publishes_a_bounded_result() {
    const auto first_root = make_temp_root();
    const auto second_root = make_temp_root();
    heartstead::save::SaveSchedulerConfig config;
    config.max_concurrent_requests = 2;
    config.max_completed_results = 2;
    auto created = heartstead::save::SaveScheduler::create(config);
    assert(created);
    auto scheduler = std::move(created).value();
    auto expensive = snapshot(55, 8U * 1024U * 1024U);
    auto cancelled_snapshot = snapshot(56);

    auto first = scheduler->submit({first_root, std::move(expensive), true});
    auto second = scheduler->submit({second_root, std::move(cancelled_snapshot), true});
    assert(first && second);
    assert(scheduler->cancel(second.value()));
    auto results = wait_for_results(*scheduler, 2);
    const auto cancelled =
        std::ranges::find(results, second.value(), &heartstead::save::SaveResult::request_id);
    assert(cancelled != results.end());
    assert(cancelled->state == heartstead::save::SaveResultState::cancelled);
    assert(!cancelled->durably_accepted);
    assert(scheduler->stats().cancelled_requests == 1);
    assert(scheduler->stats().reserved_working_bytes == 0);
    assert(!std::filesystem::exists(second_root / "journal"));
    cleanup(first_root);
    cleanup(second_root);
}

void test_invalid_and_over_budget_requests_fail_closed() {
    const auto root = make_temp_root();
    heartstead::save::SaveSchedulerConfig config;
    config.max_request_working_bytes = 128U * 1024U;
    config.max_reserved_working_bytes = 128U * 1024U;
    auto created = heartstead::save::SaveScheduler::create(config);
    assert(created);
    auto scheduler = std::move(created).value();

    auto over_budget = scheduler->submit({root, snapshot(61, 256U * 1024U), true});
    assert(!over_budget);
    assert(over_budget.error().code == "save_scheduler.request_memory_budget_exceeded");
    assert(!scheduler->has_in_flight());

    auto invalid = snapshot(62);
    invalid.chunk_edits.front().encoded_edit_delta.clear();
    // The invalid snapshot is small enough to enter the worker; durable validation still leaves
    // no finalized journal entry behind.
    auto submitted = scheduler->submit({root, std::move(invalid), true});
    assert(submitted);
    auto results = wait_for_results(*scheduler, 1);
    assert(results.front().state == heartstead::save::SaveResultState::failed);
    assert(!results.front().durably_accepted);
    assert(results.front().error_code == "save_database.empty_chunk_delta");
    auto stats = heartstead::save::FileSaveDatabase(root).stats();
    assert(stats && stats.value().journal_entry_count == 0);
    cleanup(root);
}

void test_invalid_configs_and_names_fail() {
    heartstead::save::SaveSchedulerConfig config;
    config.max_concurrent_requests = 0;
    assert(!config.validate());
    config.max_concurrent_requests = 2;
    config.max_completed_results = 1;
    assert(!config.validate());
    config.max_completed_results = 2;
    config.max_request_working_bytes = 2;
    config.max_reserved_working_bytes = 1;
    assert(!config.validate());
    assert(heartstead::save::save_result_state_name(
               static_cast<heartstead::save::SaveResultState>(255)) == std::string_view("unknown"));
}

} // namespace

int main() {
    test_memory_estimate_accounts_for_owned_payloads();
    test_background_save_is_bounded_durable_and_compacted();
    test_durable_only_result_recovers_without_compaction();
    test_slot_metadata_is_committed_on_the_worker();
    test_queued_request_cancellation_publishes_a_bounded_result();
    test_invalid_and_over_budget_requests_fail_closed();
    test_invalid_configs_and_names_fail();
    return 0;
}
