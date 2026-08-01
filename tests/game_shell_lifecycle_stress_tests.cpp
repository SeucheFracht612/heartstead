#include "engine/content/content_validation.hpp"
#include "engine/save/save_database.hpp"
#include "game/application/runtime_diagnostics.hpp"
#include "game/foundation/foundation_world.hpp"
#include "game/runtime/game_runtime.hpp"
#include "game/scenarios/developer_world_registry.hpp"

#include <cassert>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <utility>

#if defined(__has_feature)
#if __has_feature(address_sanitizer)
#define HEARTSTEAD_STRESS_ADDRESS_SANITIZER 1
#endif
#endif
#if defined(__SANITIZE_ADDRESS__)
#define HEARTSTEAD_STRESS_ADDRESS_SANITIZER 1
#endif

namespace {

using namespace heartstead;

class TemporaryDirectory final {
  public:
    TemporaryDirectory() {
        const auto suffix = std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = std::filesystem::temp_directory_path() /
                ("heartstead_shell_stress_" + std::to_string(suffix));
        std::filesystem::create_directories(path_);
    }
    ~TemporaryDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }
    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

  private:
    std::filesystem::path path_;
};

save::SaveMetadata metadata_for(const content::ContentValidationReport& report) {
    auto metadata = content::save_metadata_from_content_report(
        report, "game-shell-stress", game::foundation::world_seed);
    assert(metadata);
    return std::move(metadata).value();
}

void run_ticks(game::GameRuntime& runtime, std::int64_t& now_ms, std::string_view label,
               std::uint32_t frames = 6) {
    for (std::uint32_t frame = 0; frame < frames; ++frame) {
        now_ms += 17;
        const auto advanced = runtime.run_frame({16'667, now_ms});
        if (!advanced) {
            std::cerr << label << ": " << advanced.error().code << ": "
                      << advanced.error().message << '\n';
        }
        assert(advanced);
    }
}

void assert_clean_teardown(const game::SessionTeardownReport& report,
                           std::uint64_t generation) {
    assert(report.ownership_generation == generation);
    assert(report.invocation_count == 1);
    assert(report.rejected_new_commands);
    assert(report.transport_stopped);
    assert(report.authoritative_ticking_stopped);
    assert(report.presentation_cleared && report.presentation_objects_after == 0);
    assert(report.server_entities_after == 0);
    assert(report.physics_bodies_after == 0);
    assert(report.session_jobs_after == 0);
    assert(report.client_destroyed && report.server_destroyed);
    assert(report.completed_cleanup_count == report.registered_cleanup_count);
}

void test_repeated_world_replacement(const content::ContentValidationReport& report) {
    TemporaryDirectory temporary;
    auto registry = game::DeveloperWorldRegistry::create(report.scenario_definitions);
    assert(registry);
    auto runtime_result = game::GameRuntime::initialize({}, report);
    assert(runtime_result);
    auto runtime = std::move(runtime_result).value();
    std::int64_t now_ms = 1'000;

    // Warm allocator and content caches before taking the bounded-growth baseline.
    auto warmup = registry.value().make_launch_request("base:scenarios/foundation_slice",
                                                       metadata_for(report), true);
    assert(warmup);
    warmup.value().ownership_generation = 100;
    assert(runtime.start_session(std::move(warmup).value()));
    run_ticks(runtime, now_ms, "warmup");
    assert(runtime.shutdown());
    assert_clean_teardown(*runtime.last_teardown_report(), 100);
    const auto baseline = game::sample_process_resources();

    const auto persistent_path = temporary.path() / "persistent-world";
    for (std::uint64_t iteration = 0; iteration < 16; ++iteration) {
        const auto scenario = iteration % 2 == 0 ? "base:scenarios/foundation_slice"
                                                 : "base:scenarios/renderer_proof";
        auto request = registry.value().make_launch_request(scenario, metadata_for(report), true);
        assert(request);
        const auto generation = 101 + iteration;
        request.value().ownership_generation = generation;
        assert(runtime.start_session(std::move(request).value()));
        assert(runtime.session() != nullptr);
        const auto before = runtime.session()->resource_counts();
        assert(before.server_entities > 0);
        assert(before.presentation_objects > 0);
        std::uint64_t callback_generation = 0;
        assert(runtime.session()->register_cleanup("stress generation callback",
                                                   [&callback_generation, generation]() {
                                                       callback_generation = generation;
                                                       return core::Status::ok();
                                                   }));
        assert(runtime.session()->resource_counts().registered_cleanup_callbacks == 1);
        run_ticks(runtime, now_ms, scenario);
        if (iteration == 0) {
            assert(runtime.save_to(save::FileSaveDatabase(persistent_path)));
        }
        assert(runtime.shutdown());
        assert(callback_generation == generation);
        assert(runtime.session() == nullptr);
        assert_clean_teardown(*runtime.last_teardown_report(), generation);
    }

    auto persistent = game::SessionLaunchRequest{};
    persistent.ownership_generation = 117;
    persistent.mode = game::SessionMode::local_single_player;
    persistent.world_source = game::WorldSourceKind::existing_save;
    persistent.persistence = game::PersistencePolicy::persistent;
    persistent.world_name = "Persistent stress world";
    persistent.scenario_id.clear();
    persistent.save_path = persistent_path;
    persistent.metadata = metadata_for(report);
    persistent.runtime.headless = true;
    assert(runtime.start_session(std::move(persistent)));
    run_ticks(runtime, now_ms, "persistent reload");
    assert(runtime.save_to(save::FileSaveDatabase(persistent_path)));
    assert(runtime.shutdown());
    assert_clean_teardown(*runtime.last_teardown_report(), 117);

    const auto after = game::sample_process_resources();
#if defined(__linux__)
    assert(baseline.resident_memory_bytes.has_value() && after.resident_memory_bytes.has_value());
#if !defined(HEARTSTEAD_STRESS_ADDRESS_SANITIZER)
    assert(*after.resident_memory_bytes <=
           *baseline.resident_memory_bytes + 128U * 1024U * 1024U);
#endif
    assert(baseline.thread_count.has_value() && after.thread_count.has_value());
    assert(*after.thread_count <= *baseline.thread_count + 2U);
    assert(baseline.open_file_count.has_value() && after.open_file_count.has_value());
    assert(*after.open_file_count <= *baseline.open_file_count + 4U);
    std::cout << "lifecycle stress: 17 replacements, RSS "
              << *baseline.resident_memory_bytes << " -> " << *after.resident_memory_bytes
              << " bytes, threads " << *baseline.thread_count << " -> " << *after.thread_count
              << ", open files " << *baseline.open_file_count << " -> "
              << *after.open_file_count << '\n';
#else
    std::cout << "lifecycle stress: 17 replacements; process counters unavailable\n";
#endif
    assert(runtime.shutdown());
}

} // namespace

int main() {
    const auto report =
        heartstead::content::ContentValidation::validate(HEARTSTEAD_TEST_SOURCE_DIR);
    assert(!report.has_errors());
    test_repeated_world_replacement(report);
    return 0;
}
