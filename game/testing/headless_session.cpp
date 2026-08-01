#include "game/testing/headless_session.hpp"

#include "engine/content/content_validation.hpp"

#include <algorithm>
#include <limits>
#include <utility>

namespace heartstead::game {

HeadlessSessionHarness::HeadlessSessionHarness(GameRuntime runtime)
    : runtime_(std::move(runtime)) {}

HeadlessSessionHarness::~HeadlessSessionHarness() {
    (void)shutdown();
}

core::Result<std::unique_ptr<HeadlessSessionHarness>>
HeadlessSessionHarness::create(HeadlessSessionDesc desc) {
    if (desc.source_root.empty()) {
        return core::Result<std::unique_ptr<HeadlessSessionHarness>>::failure(
            "headless_session.missing_source_root",
            "headless test session requires a content source root");
    }
    if (!desc.runtime.headless) {
        return core::Result<std::unique_ptr<HeadlessSessionHarness>>::failure(
            "headless_session.invalid_runtime",
            "headless test sessions require a headless runtime configuration");
    }
    const auto content_report = content::ContentValidation::validate(desc.source_root);
    if (content_report.has_errors()) {
        return core::Result<std::unique_ptr<HeadlessSessionHarness>>::failure(
            "headless_session.content_invalid", "headless test content validation failed");
    }
    auto runtime = GameRuntime::initialize(std::move(desc.game), content_report);
    if (!runtime) {
        return core::Result<std::unique_ptr<HeadlessSessionHarness>>::failure(
            runtime.error().code, runtime.error().message);
    }
    SessionRequest request;
    if (desc.initial_snapshot.has_value()) {
        request.metadata = desc.initial_snapshot->metadata;
        request.initial_snapshot = std::move(desc.initial_snapshot);
    } else {
        auto metadata = content::save_metadata_from_content_report(
            content_report, std::move(desc.world_name), desc.world_seed);
        if (!metadata) {
            return core::Result<std::unique_ptr<HeadlessSessionHarness>>::failure(
                metadata.error().code, metadata.error().message);
        }
        request.metadata = std::move(metadata).value();
    }
    request.scenario_id = runtime.value().startup_report().selected_scenario_id;
    auto status = runtime.value().start_session(std::move(desc.runtime), std::move(request));
    if (!status) {
        return core::Result<std::unique_ptr<HeadlessSessionHarness>>::failure(
            status.error().code, status.error().message);
    }
    return core::Result<std::unique_ptr<HeadlessSessionHarness>>::success(
        std::unique_ptr<HeadlessSessionHarness>(
            new HeadlessSessionHarness(std::move(runtime).value())));
}

core::Result<HeadlessRunReport>
HeadlessSessionHarness::run_ticks(std::uint32_t tick_count) {
    if (runtime_.session() == nullptr) {
        return core::Result<HeadlessRunReport>::failure(
            "headless_session.not_running", "headless test session is not running");
    }
    HeadlessRunReport report;
    report.requested_tick_count = tick_count;
    const auto tick_rate = runtime_.session()->config().fixed_step.ticks_per_second;
    const auto frame_us = std::max<std::uint64_t>(1, 1'000'000ULL / tick_rate);
    const auto maximum_frames = static_cast<std::uint64_t>(tick_count) * 2ULL + 2ULL;
    while (report.completed_tick_count < tick_count && report.frame_count < maximum_frames) {
        if (elapsed_us_ > std::numeric_limits<std::uint64_t>::max() - frame_us) {
            return core::Result<HeadlessRunReport>::failure(
                "headless_session.clock_exhausted", "headless test clock is exhausted");
        }
        elapsed_us_ += frame_us;
        auto frame = runtime_.run_frame(
            {frame_us, static_cast<std::int64_t>(elapsed_us_ / 1'000ULL)});
        if (!frame) {
            return core::Result<HeadlessRunReport>::failure(frame.error().code,
                                                            frame.error().message);
        }
        report.completed_tick_count += frame.value().fixed_step.step_count;
        report.last_frame = std::move(frame).value();
        ++report.frame_count;
    }
    if (report.completed_tick_count != tick_count) {
        return core::Result<HeadlessRunReport>::failure(
            "headless_session.tick_budget_failed",
            "headless session could not advance the requested deterministic tick count");
    }
    return core::Result<HeadlessRunReport>::success(std::move(report));
}

core::Status HeadlessSessionHarness::shutdown() {
    return runtime_.shutdown();
}

GameRuntime& HeadlessSessionHarness::runtime() noexcept {
    return runtime_;
}

const GameRuntime& HeadlessSessionHarness::runtime() const noexcept {
    return runtime_;
}

} // namespace heartstead::game
