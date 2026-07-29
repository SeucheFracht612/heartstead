#pragma once

#include "engine/core/ids.hpp"
#include "engine/core/result.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <queue>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace heartstead::platform {

struct WindowIdTag;
using WindowId = core::StrongU64Id<WindowIdTag>;

enum class PlatformBackend {
    headless,
    native,
};

enum class NativeWindowSystem {
    none,
    x11,
};

struct NativeWindowHandle {
    NativeWindowSystem system = NativeWindowSystem::none;
    void* display = nullptr;
    std::uint64_t window = 0;
};

struct PlatformBackendInfo {
    PlatformBackend backend = PlatformBackend::headless;
    std::string_view name;
    bool available = false;
    std::string_view status;
};

struct PlatformBackendCapabilities {
    PlatformBackend backend = PlatformBackend::headless;
    bool available = false;
    bool headless = true;
    bool supports_logical_windows = false;
    bool supports_native_windows = false;
    bool supports_keyboard_input = false;
    bool supports_text_input = false;
    bool supports_mouse_input = false;
    bool supports_display_metadata = false;
    bool supports_vulkan_surface = false;
    bool supports_clipboard = false;
    std::string_view window_system;
};

struct PlatformDesc {
    PlatformBackend backend = PlatformBackend::headless;
};

struct WindowDesc {
    std::string title = "Heartstead";
    std::uint32_t width = 1280;
    std::uint32_t height = 720;
    bool resizable = true;
};

struct WindowState {
    WindowId id;
    std::string title;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    bool resizable = true;
    bool open = false;
};

struct DisplayInfo {
    std::uint32_t index = 0;
    std::string name;
    std::int32_t x_px = 0;
    std::int32_t y_px = 0;
    std::uint32_t width_px = 0;
    std::uint32_t height_px = 0;
    std::uint32_t width_mm = 0;
    std::uint32_t height_mm = 0;
    double dpi_x = 0.0;
    double dpi_y = 0.0;
    double refresh_hz = 0.0;
    bool primary = false;
};

enum class KeyCode {
    unknown,
    escape,
    enter,
    tab,
    space,
    backspace,
    delete_key,
    arrow_left,
    arrow_right,
    arrow_up,
    arrow_down,
    digit_1,
    digit_2,
    digit_3,
    digit_4,
    digit_5,
    digit_6,
    digit_7,
    digit_8,
    digit_9,
    w,
    a,
    s,
    d,
    q,
    e,
    r,
    left_shift,
    left_control,
    left_alt,
    f1,
    f3,
    f4,
    f5,
};

enum class MouseButton {
    unknown,
    left,
    right,
    middle,
    x1,
    x2,
};

enum class PlatformEventKind {
    quit_requested,
    window_created,
    window_closed,
    window_resized,
    window_focus_gained,
    window_focus_lost,
    key_down,
    key_up,
    text_input,
    mouse_moved,
    mouse_button_down,
    mouse_button_up,
    mouse_wheel,
};

struct PlatformEvent {
    PlatformEventKind kind = PlatformEventKind::quit_requested;
    WindowId window_id;
    KeyCode key = KeyCode::unknown;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::string text;
    MouseButton mouse_button = MouseButton::unknown;
    std::int32_t mouse_x = 0;
    std::int32_t mouse_y = 0;
    std::int32_t wheel_delta_x = 0;
    std::int32_t wheel_delta_y = 0;
    std::int32_t mouse_delta_x = 0;
    std::int32_t mouse_delta_y = 0;
};

struct KeyInputState {
    bool down = false;
    bool pressed = false;
    bool released = false;
};

struct MouseButtonState {
    bool down = false;
    bool pressed = false;
    bool released = false;
};

struct MousePosition {
    std::int32_t x = 0;
    std::int32_t y = 0;
    bool inside = false;
};

struct WindowInputSnapshot {
    WindowId window_id;
    std::vector<KeyCode> down_keys;
    std::vector<KeyCode> pressed_keys;
    std::vector<KeyCode> released_keys;
    std::vector<MouseButton> down_mouse_buttons;
    std::vector<MouseButton> pressed_mouse_buttons;
    std::vector<MouseButton> released_mouse_buttons;
    MousePosition mouse;
    std::int32_t mouse_delta_x = 0;
    std::int32_t mouse_delta_y = 0;
    std::int32_t wheel_delta_x = 0;
    std::int32_t wheel_delta_y = 0;
    std::vector<std::string> text;
    bool cursor_captured = false;
};

struct AppRunConfig {
    std::uint64_t max_frames = 1;
};

class PlatformClock {
  public:
    PlatformClock();

    [[nodiscard]] std::int64_t now_ms() const noexcept;
    [[nodiscard]] std::int64_t elapsed_ms() const noexcept;

  private:
    std::int64_t start_ms_ = 0;
};

class IPlatform {
  public:
    virtual ~IPlatform() = default;

    [[nodiscard]] virtual PlatformBackend backend() const noexcept = 0;
    [[nodiscard]] virtual std::string_view backend_name() const noexcept = 0;
    [[nodiscard]] virtual PlatformBackendCapabilities capabilities() const noexcept = 0;
    [[nodiscard]] virtual std::vector<DisplayInfo> displays() const = 0;
    [[nodiscard]] virtual core::Status set_clipboard_text(std::string text) = 0;
    [[nodiscard]] virtual core::Result<std::string> clipboard_text() const = 0;

    [[nodiscard]] virtual core::Result<WindowId> create_window(const WindowDesc& desc) = 0;
    [[nodiscard]] virtual core::Status close_window(WindowId id) = 0;
    [[nodiscard]] virtual const WindowState* find_window(WindowId id) const noexcept = 0;
    [[nodiscard]] virtual std::optional<NativeWindowHandle>
    native_window_handle(WindowId id) const noexcept = 0;
    [[nodiscard]] virtual std::size_t open_window_count() const noexcept = 0;

    virtual void begin_frame() = 0;
    [[nodiscard]] virtual core::Status queue_event(PlatformEvent event) = 0;
    [[nodiscard]] virtual std::optional<PlatformEvent> poll_event() = 0;

    [[nodiscard]] virtual std::optional<KeyInputState> key_state(WindowId window_id,
                                                                 KeyCode key) const noexcept = 0;
    [[nodiscard]] virtual std::optional<MouseButtonState>
    mouse_button_state(WindowId window_id, MouseButton button) const noexcept = 0;
    [[nodiscard]] virtual std::optional<WindowInputSnapshot>
    input_snapshot(WindowId window_id) const noexcept = 0;
    [[nodiscard]] virtual core::Status set_cursor_capture(WindowId window_id, bool captured) = 0;
    [[nodiscard]] virtual bool cursor_captured(WindowId window_id) const noexcept = 0;
    [[nodiscard]] virtual bool is_key_down(WindowId window_id, KeyCode key) const noexcept = 0;
    [[nodiscard]] virtual bool was_key_pressed(WindowId window_id, KeyCode key) const noexcept = 0;
    [[nodiscard]] virtual bool was_key_released(WindowId window_id, KeyCode key) const noexcept = 0;
    [[nodiscard]] virtual bool is_mouse_button_down(WindowId window_id,
                                                    MouseButton button) const noexcept = 0;
    [[nodiscard]] virtual bool was_mouse_button_pressed(WindowId window_id,
                                                        MouseButton button) const noexcept = 0;
    [[nodiscard]] virtual bool was_mouse_button_released(WindowId window_id,
                                                         MouseButton button) const noexcept = 0;

    virtual void request_quit() = 0;
    [[nodiscard]] virtual bool should_quit() const noexcept = 0;
    [[nodiscard]] virtual PlatformClock& clock() noexcept = 0;
    [[nodiscard]] virtual const PlatformClock& clock() const noexcept = 0;
};

class HeadlessPlatform final : public IPlatform {
  public:
    [[nodiscard]] PlatformBackend backend() const noexcept override;
    [[nodiscard]] std::string_view backend_name() const noexcept override;
    [[nodiscard]] PlatformBackendCapabilities capabilities() const noexcept override;
    [[nodiscard]] std::vector<DisplayInfo> displays() const override;
    [[nodiscard]] core::Status set_clipboard_text(std::string text) override;
    [[nodiscard]] core::Result<std::string> clipboard_text() const override;

    [[nodiscard]] core::Result<WindowId> create_window(const WindowDesc& desc) override;
    [[nodiscard]] core::Status close_window(WindowId id) override;
    [[nodiscard]] const WindowState* find_window(WindowId id) const noexcept override;
    [[nodiscard]] std::optional<NativeWindowHandle>
    native_window_handle(WindowId id) const noexcept override;
    [[nodiscard]] std::size_t open_window_count() const noexcept override;

    void begin_frame() override;
    [[nodiscard]] core::Status queue_event(PlatformEvent event) override;
    [[nodiscard]] std::optional<PlatformEvent> poll_event() override;

    [[nodiscard]] std::optional<KeyInputState> key_state(WindowId window_id,
                                                         KeyCode key) const noexcept override;
    [[nodiscard]] std::optional<MouseButtonState>
    mouse_button_state(WindowId window_id, MouseButton button) const noexcept override;
    [[nodiscard]] std::optional<WindowInputSnapshot>
    input_snapshot(WindowId window_id) const noexcept override;
    [[nodiscard]] core::Status set_cursor_capture(WindowId window_id, bool captured) override;
    [[nodiscard]] bool cursor_captured(WindowId window_id) const noexcept override;
    [[nodiscard]] bool is_key_down(WindowId window_id, KeyCode key) const noexcept override;
    [[nodiscard]] bool was_key_pressed(WindowId window_id, KeyCode key) const noexcept override;
    [[nodiscard]] bool was_key_released(WindowId window_id, KeyCode key) const noexcept override;
    [[nodiscard]] bool is_mouse_button_down(WindowId window_id,
                                            MouseButton button) const noexcept override;
    [[nodiscard]] bool was_mouse_button_pressed(WindowId window_id,
                                                MouseButton button) const noexcept override;
    [[nodiscard]] bool was_mouse_button_released(WindowId window_id,
                                                 MouseButton button) const noexcept override;

    void request_quit() override;
    [[nodiscard]] bool should_quit() const noexcept override;
    [[nodiscard]] PlatformClock& clock() noexcept override;
    [[nodiscard]] const PlatformClock& clock() const noexcept override;

  private:
    WindowId next_window_id();
    void emit_event(PlatformEvent event);
    [[nodiscard]] core::Status validate_queued_event(const PlatformEvent& event) const;
    void apply_input_event(const PlatformEvent& event);

    std::uint64_t next_window_id_ = 1;
    bool quit_requested_ = false;
    std::string clipboard_text_;
    PlatformClock clock_;
    std::unordered_map<std::uint64_t, WindowState> windows_;
    std::unordered_map<std::uint64_t, std::unordered_map<std::uint32_t, KeyInputState>> input_;
    std::unordered_map<std::uint64_t, std::unordered_map<std::uint32_t, MouseButtonState>>
        mouse_buttons_;
    std::unordered_map<std::uint64_t, MousePosition> mouse_positions_;
    std::unordered_map<std::uint64_t, std::pair<std::int32_t, std::int32_t>> mouse_delta_;
    std::unordered_map<std::uint64_t, std::pair<std::int32_t, std::int32_t>> mouse_wheel_;
    std::unordered_map<std::uint64_t, std::vector<std::string>> text_input_;
    std::unordered_map<std::uint64_t, bool> cursor_captured_;
    std::queue<PlatformEvent> events_;
};

using HeadlessFrameCallback = std::function<core::Status(
    HeadlessPlatform& platform, std::uint64_t frame_index, std::int64_t now_ms)>;
using PlatformFrameCallback = std::function<core::Status(
    IPlatform& platform, std::uint64_t frame_index, std::int64_t now_ms)>;

[[nodiscard]] core::Status run_headless_app(HeadlessPlatform& platform, const AppRunConfig& config,
                                            const HeadlessFrameCallback& frame_callback);
[[nodiscard]] core::Status run_platform_app(IPlatform& platform, const AppRunConfig& config,
                                            const PlatformFrameCallback& frame_callback);

[[nodiscard]] core::Result<std::unique_ptr<IPlatform>> create_platform(PlatformDesc desc = {});
[[nodiscard]] core::Status validate_platform_desc(const PlatformDesc& desc);
[[nodiscard]] core::Status validate_window_desc(const WindowDesc& desc);
[[nodiscard]] PlatformBackendInfo platform_backend_info(PlatformBackend backend) noexcept;
[[nodiscard]] PlatformBackendCapabilities
platform_backend_capabilities(PlatformBackend backend) noexcept;
[[nodiscard]] std::string_view platform_backend_name(PlatformBackend backend) noexcept;
[[nodiscard]] std::string_view platform_event_kind_name(PlatformEventKind kind) noexcept;
[[nodiscard]] std::string_view key_code_name(KeyCode key) noexcept;
[[nodiscard]] std::string_view mouse_button_name(MouseButton button) noexcept;

} // namespace heartstead::platform
