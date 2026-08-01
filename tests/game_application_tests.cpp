#include "game/application/game_application.hpp"
#include "game/application/startup_recovery_mode.hpp"

#include <cassert>
#include <csignal>
#include <cstdint>
#include <string>

namespace {

using namespace heartstead;

class RecordingMode final : public game::IGameApplicationMode {
  public:
    core::Status initialize(game::GameApplicationServices& services) override {
        assert(services.headless());
        assert(services.renderer() == nullptr);
        assert(services.audio() == nullptr);
        assert(services.jobs() != nullptr);
        assert(services.jobs()->backend() == jobs::JobBackend::immediate);
        initialized = true;
        return core::Status::ok();
    }

    core::Result<game::GameApplicationFrameOutput>
    update(game::GameApplicationServices&, const game::GameApplicationFrame& frame) override {
        assert(initialized);
        assert(!shutdown_called);
        assert(frame.headless);
        assert(frame.input == nullptr);
        assert(frame.delta_microseconds == 16'667);
        assert(frame.now_milliseconds > previous_now_milliseconds);
        assert(frame.wall_clock_milliseconds > 1'500'000'000'000LL);
        previous_now_milliseconds = frame.now_milliseconds;
        ++updates;
        return core::Result<game::GameApplicationFrameOutput>::success({});
    }

    core::Status shutdown(game::GameApplicationServices& services) override {
        assert(services.headless());
        shutdown_called = true;
        return core::Status::ok();
    }

    std::string summary() const override {
        return "recording updates=" + std::to_string(updates);
    }

    bool initialized = false;
    bool shutdown_called = false;
    std::uint64_t updates = 0;
    std::int64_t previous_now_milliseconds = -1;
};

class FailingMode final : public game::IGameApplicationMode {
  public:
    core::Status initialize(game::GameApplicationServices&) override {
        initialized = true;
        return core::Status::ok();
    }

    core::Result<game::GameApplicationFrameOutput>
    update(game::GameApplicationServices&, const game::GameApplicationFrame&) override {
        return core::Result<game::GameApplicationFrameOutput>::failure("test.mode_failure",
                                                                       "expected mode failure");
    }

    core::Status shutdown(game::GameApplicationServices&) override {
        shutdown_called = true;
        return core::Status::ok();
    }

    std::string summary() const override {
        return "failing";
    }

    bool initialized = false;
    bool shutdown_called = false;
};

class SignalStoppingMode final : public game::IGameApplicationMode {
  public:
    core::Status initialize(game::GameApplicationServices&) override {
        return core::Status::ok();
    }

    core::Result<game::GameApplicationFrameOutput>
    update(game::GameApplicationServices&, const game::GameApplicationFrame&) override {
        ++updates;
        assert(std::raise(SIGINT) == 0);
        return core::Result<game::GameApplicationFrameOutput>::success({});
    }

    core::Status shutdown(game::GameApplicationServices&) override {
        shutdown_called = true;
        return core::Status::ok();
    }

    std::string summary() const override {
        return "signal stopping";
    }

    std::uint64_t updates = 0;
    bool shutdown_called = false;
};

void test_headless_loop_and_lifecycle() {
    game::GameApplicationConfig config;
    config.headless = true;
    config.maximum_frames = 3;
    game::GameApplication application(config);
    RecordingMode mode;

    auto report = application.run(mode);
    assert(report);
    assert(report.value().headless);
    assert(report.value().frame_count == 3);
    assert(report.value().mode_summary == "recording updates=3");
    assert(mode.initialized);
    assert(mode.shutdown_called);
    assert(mode.updates == 3);
}

void test_mode_failure_still_shuts_down() {
    game::GameApplicationConfig config;
    config.headless = true;
    config.maximum_frames = 1;
    game::GameApplication application(config);
    FailingMode mode;

    auto report = application.run(mode);
    assert(!report);
    assert(report.error().code == "test.mode_failure");
    assert(mode.initialized);
    assert(mode.shutdown_called);
}

void test_process_signal_uses_orderly_shutdown() {
    game::GameApplicationConfig config;
    config.headless = true;
    config.maximum_frames = 10;
    game::GameApplication application(config);
    SignalStoppingMode mode;

    auto report = application.run(mode);
    assert(report);
    assert(report.value().frame_count == 1);
    assert(mode.updates == 1);
    assert(mode.shutdown_called);
}

void test_zero_frame_limit_is_rejected() {
    game::GameApplicationConfig config;
    config.headless = true;
    config.maximum_frames = 0;
    game::GameApplication application(config);
    RecordingMode mode;

    auto report = application.run(mode);
    assert(!report);
    assert(report.error().code == "game_application.invalid_frame_limit");
    assert(!mode.initialized);
}

void test_zero_application_workers_are_rejected() {
    game::GameApplicationConfig config;
    config.headless = true;
    config.maximum_frames = 1;
    config.application_worker_count = 0;
    game::GameApplication application(config);
    RecordingMode mode;

    auto report = application.run(mode);
    assert(!report);
    assert(report.error().code == "game_application.invalid_worker_count");
    assert(!mode.initialized);
}

void test_zero_frame_delta_limit_is_rejected() {
    game::GameApplicationConfig config;
    config.headless = true;
    config.maximum_frames = 1;
    config.maximum_frame_delta_microseconds = 0;
    game::GameApplication application(config);
    RecordingMode mode;

    auto report = application.run(mode);
    assert(!report);
    assert(report.error().code == "game_application.invalid_frame_delta_limit");
    assert(!mode.initialized);
}

void test_headless_startup_recovery_needs_no_game_runtime() {
    game::GameApplicationConfig config;
    config.headless = true;
    config.maximum_frames = 2;
    game::GameApplication application(config);
    game::StartupRecoveryMode mode({{{modding::DiagnosticSeverity::error,
                                      "broken-content.txt",
                                      "content.test_failure",
                                      "expected validation failure"}},
                                    true});

    auto report = application.run(mode);
    assert(report);
    assert(report.value().frame_count == 2);
    assert(report.value().mode_summary == "startup recovery: diagnostics=1 frames=2");
}

void test_native_minimize_preserves_committed_extent() {
    const renderer::rhi::RenderExtent committed{1280, 720};
    const auto minimized = game::resolve_application_window_resize(committed, {0, 0});
    assert(minimized.minimized);
    assert(!minimized.resize_renderer);
    assert(minimized.extent.width == committed.width);
    assert(minimized.extent.height == committed.height);

    const auto unchanged_restore =
        game::resolve_application_window_resize(minimized.extent, committed);
    assert(!unchanged_restore.minimized);
    assert(!unchanged_restore.resize_renderer);

    const auto resized = game::resolve_application_window_resize(committed, {1920, 1080});
    assert(!resized.minimized);
    assert(resized.resize_renderer);
    assert(resized.extent.width == 1920);
    assert(resized.extent.height == 1080);
}

} // namespace

int main() {
    test_headless_loop_and_lifecycle();
    test_mode_failure_still_shuts_down();
    test_process_signal_uses_orderly_shutdown();
    test_zero_frame_limit_is_rejected();
    test_zero_application_workers_are_rejected();
    test_zero_frame_delta_limit_is_rejected();
    test_headless_startup_recovery_needs_no_game_runtime();
    test_native_minimize_preserves_committed_extent();
    return 0;
}
