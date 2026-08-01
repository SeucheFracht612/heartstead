#include "game/application/startup_recovery_mode.hpp"

#include "engine/ui/widget_tree.hpp"
#include "game/application/application_settings.hpp"

#include <algorithm>
#include <string>

namespace heartstead::game {

namespace {

const auto root_id = ui::widget_id("heartstead.recovery.root");
const auto panel_id = ui::widget_id("heartstead.recovery.panel");
const auto quit_id = ui::widget_id("heartstead.recovery.quit");

[[nodiscard]] ui::WidgetDesc label(std::string_view id, std::string text,
                                   float glyph_size = 16.0F) {
    ui::WidgetDesc result;
    result.id = ui::widget_id(id);
    result.parent = panel_id;
    result.kind = ui::WidgetKind::label;
    result.layout.width = ui::UiSize::fill();
    result.layout.height = ui::UiSize::content();
    result.layout.padding = {8.0F, 5.0F, 8.0F, 5.0F};
    result.text = std::move(text);
    result.glyph_size_pixels = glyph_size;
    return result;
}

} // namespace

struct StartupRecoveryMode::Impl {
    explicit Impl(StartupRecoveryModeConfig initial_config)
        : config(std::move(initial_config)), widgets(ui::UiSkin::storybook_default()) {}

    StartupRecoveryModeConfig config;
    ui::WidgetTree widgets;
    std::uint64_t frame_count = 0;
    bool initialized = false;

    [[nodiscard]] core::Status build_ui() {
        ui::WidgetDesc root;
        root.id = root_id;
        root.kind = ui::WidgetKind::panel;
        root.layout.width = ui::UiSize::fill();
        root.layout.height = ui::UiSize::fill();
        root.color = {0.025F, 0.035F, 0.05F, 1.0F};
        root.blocks_gameplay = true;
        auto status = widgets.add(std::move(root));
        if (!status) {
            return status;
        }

        ui::WidgetDesc panel;
        panel.id = panel_id;
        panel.parent = root_id;
        panel.kind = ui::WidgetKind::scroll_area;
        panel.nine_slice = "carved_panel";
        panel.layout.mode = ui::UiLayoutMode::column;
        panel.layout.width = ui::UiSize::pixels(620.0F);
        panel.layout.height = ui::UiSize::content();
        panel.layout.minimum_height = 260.0F;
        panel.layout.maximum_height = 620.0F;
        panel.layout.padding = {30.0F, 28.0F, 30.0F, 28.0F};
        panel.layout.gap = 12.0F;
        panel.layout.horizontal_alignment = ui::UiAlignment::center;
        panel.layout.vertical_alignment = ui::UiAlignment::center;
        panel.color = {0.22F, 0.09F, 0.055F, 0.98F};
        status = widgets.add(std::move(panel));
        if (!status) {
            return status;
        }

        status = widgets.add(label("heartstead.recovery.title", "Heartstead recovery", 30.0F));
        if (status) {
            status = widgets.add(label(
                "heartstead.recovery.summary",
                "Gameplay content could not be validated. Worlds were not opened or modified."));
        }
        if (!status) {
            return status;
        }

        std::size_t displayed = 0;
        for (const auto& diagnostic : config.diagnostics) {
            if (diagnostic.severity != modding::DiagnosticSeverity::error || displayed >= 8U) {
                continue;
            }
            auto source = diagnostic.source.filename().string();
            if (source.empty()) {
                source = "content";
            }
            status = widgets.add(label(
                "heartstead.recovery.diagnostic." + std::to_string(displayed),
                diagnostic.code + " (" + source + "): " + diagnostic.message, 13.0F));
            if (!status) {
                return status;
            }
            ++displayed;
        }
        if (displayed == 0U) {
            status = widgets.add(label("heartstead.recovery.no_details",
                                       "No detailed validation diagnostic was available."));
            if (!status) {
                return status;
            }
        }

        ui::WidgetDesc quit;
        quit.id = quit_id;
        quit.parent = panel_id;
        quit.kind = ui::WidgetKind::button;
        quit.nine_slice = "carved_button";
        quit.layout.width = ui::UiSize::fill();
        quit.layout.height = ui::UiSize::pixels(48.0F);
        quit.layout.padding = {16.0F, 14.0F, 16.0F, 8.0F};
        quit.text = "Quit";
        quit.focusable = true;
        quit.pointer_events = true;
        quit.color = {0.42F, 0.16F, 0.09F, 1.0F};
        status = widgets.add(std::move(quit));
        if (status) {
            widgets.set_focus(quit_id);
        }
        return status;
    }
};

StartupRecoveryMode::StartupRecoveryMode(StartupRecoveryModeConfig config)
    : implementation_(std::make_unique<Impl>(std::move(config))) {}

StartupRecoveryMode::~StartupRecoveryMode() = default;

core::Status StartupRecoveryMode::initialize(GameApplicationServices&) {
    auto& state = *implementation_;
    auto status = state.build_ui();
    if (status) {
        state.initialized = true;
    }
    return status;
}

core::Result<GameApplicationFrameOutput>
StartupRecoveryMode::update(GameApplicationServices& services, const GameApplicationFrame& frame) {
    auto& state = *implementation_;
    if (!state.initialized) {
        return core::Result<GameApplicationFrameOutput>::failure(
            "startup_recovery.not_initialized", "startup recovery mode was not initialized");
    }
    ++state.frame_count;
    if (state.config.headless || frame.headless) {
        return core::Result<GameApplicationFrameOutput>::success({});
    }
    if (!frame.extent.is_valid()) {
        return core::Result<GameApplicationFrameOutput>::success({});
    }
    const auto scale = effective_application_ui_scale(frame.extent.width, frame.extent.height,
                                                      state.config.ui_scale);
    auto status = state.widgets.layout(
        {static_cast<float>(frame.extent.width), static_cast<float>(frame.extent.height)}, scale);
    if (!status) {
        return core::Result<GameApplicationFrameOutput>::failure(status.error().code,
                                                                 status.error().message);
    }
    if (frame.input != nullptr) {
        const auto routed = state.widgets.route_input(ui::UiInputFrame::from_platform(*frame.input));
        if (routed.events.end() !=
            std::ranges::find_if(routed.events, [](const ui::UiEvent& event) {
                return event.kind == ui::UiEventKind::cancelled ||
                       (event.kind == ui::UiEventKind::clicked && event.target == quit_id);
            })) {
            services.request_quit();
        }
    }
    if (services.renderer() == nullptr || services.renderer()->ui_renderer() == nullptr) {
        return core::Result<GameApplicationFrameOutput>::failure(
            "startup_recovery.renderer_unavailable",
            "startup recovery UI requires the core renderer and UI pipeline");
    }
    auto painted = state.widgets.paint(*services.renderer()->ui_renderer());
    if (!painted) {
        return core::Result<GameApplicationFrameOutput>::failure(painted.error().code,
                                                                 painted.error().message);
    }

    renderer::RenderCamera camera;
    status = camera.set_aspect_ratio(static_cast<float>(frame.extent.width) /
                                     static_cast<float>(frame.extent.height));
    if (!status) {
        return core::Result<GameApplicationFrameOutput>::failure(status.error().code,
                                                                 status.error().message);
    }
    GameApplicationFrameOutput output;
    output.render = renderer::RenderFrameInput{camera, 1.0F, frame.delta_seconds()};
    return core::Result<GameApplicationFrameOutput>::success(std::move(output));
}

core::Status StartupRecoveryMode::shutdown(GameApplicationServices&) {
    implementation_->widgets.clear();
    implementation_->initialized = false;
    return core::Status::ok();
}

std::string StartupRecoveryMode::summary() const {
    return "startup recovery: diagnostics=" +
           std::to_string(implementation_->config.diagnostics.size()) +
           " frames=" + std::to_string(implementation_->frame_count);
}

} // namespace heartstead::game
