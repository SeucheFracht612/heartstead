#include "engine/platform/platform.hpp"

#if HEARTSTEAD_HAS_X11
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#if HEARTSTEAD_HAS_XRANDR
#include <X11/extensions/Xrandr.h>
#endif
#ifdef Status
#undef Status
#endif
#endif

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <limits>
#include <thread>
#include <unordered_set>
#include <utility>

namespace heartstead::platform {

namespace {

[[nodiscard]] std::int64_t monotonic_ms() noexcept {
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    return std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
}

[[nodiscard]] bool is_window_open(const std::unordered_map<std::uint64_t, WindowState>& windows,
                                  WindowId id) noexcept {
    const auto found = windows.find(id.value());
    return found != windows.end() && found->second.open;
}

void sort_keys(std::vector<KeyCode>& keys) {
    std::ranges::sort(keys, [](KeyCode lhs, KeyCode rhs) {
        return static_cast<std::uint32_t>(lhs) < static_cast<std::uint32_t>(rhs);
    });
}

void sort_mouse_buttons(std::vector<MouseButton>& buttons) {
    std::ranges::sort(buttons, [](MouseButton lhs, MouseButton rhs) {
        return static_cast<std::uint32_t>(lhs) < static_cast<std::uint32_t>(rhs);
    });
}

[[nodiscard]] DisplayInfo make_headless_display_info() {
    DisplayInfo info;
    info.index = 0;
    info.name = "headless";
    info.width_px = 1280;
    info.height_px = 720;
    info.dpi_x = 96.0;
    info.dpi_y = 96.0;
    info.primary = true;
    return info;
}

#if HEARTSTEAD_HAS_X11

struct X11SelectionProperty {
    Atom type = None;
    int format = 0;
    unsigned long item_count = 0;
    std::vector<unsigned char> bytes;
};

struct X11OutgoingIncrementalClipboardTransfer {
    ::Window requestor = 0;
    Atom property = None;
    Atom property_type = None;
    std::string bytes;
    std::size_t offset = 0;
};

[[nodiscard]] bool x11_display_available() noexcept {
    Display* display = XOpenDisplay(nullptr);
    if (display == nullptr) {
        return false;
    }
    XCloseDisplay(display);
    return true;
}

[[nodiscard]] const char* x11_pointer_grab_result_name(int result) noexcept {
    switch (result) {
    case GrabSuccess:
        return "GrabSuccess";
    case AlreadyGrabbed:
        return "AlreadyGrabbed";
    case GrabInvalidTime:
        return "GrabInvalidTime";
    case GrabNotViewable:
        return "GrabNotViewable";
    case GrabFrozen:
        return "GrabFrozen";
    default:
        return "unknown";
    }
}

[[nodiscard]] bool x11_pointer_grab_result_is_transient(int result) noexcept {
    return result == AlreadyGrabbed || result == GrabNotViewable || result == GrabFrozen;
}

[[nodiscard]] KeyCode key_from_x11_keysym(KeySym key) noexcept {
    switch (key) {
    case XK_Escape:
        return KeyCode::escape;
    case XK_Return:
    case XK_KP_Enter:
        return KeyCode::enter;
    case XK_Tab:
        return KeyCode::tab;
    case XK_space:
        return KeyCode::space;
    case XK_BackSpace:
        return KeyCode::backspace;
    case XK_Delete:
        return KeyCode::delete_key;
    case XK_Left:
    case XK_KP_Left:
        return KeyCode::arrow_left;
    case XK_Right:
    case XK_KP_Right:
        return KeyCode::arrow_right;
    case XK_Up:
    case XK_KP_Up:
        return KeyCode::arrow_up;
    case XK_Down:
    case XK_KP_Down:
        return KeyCode::arrow_down;
    case XK_1:
    case XK_KP_1:
        return KeyCode::digit_1;
    case XK_2:
    case XK_KP_2:
        return KeyCode::digit_2;
    case XK_3:
    case XK_KP_3:
        return KeyCode::digit_3;
    case XK_4:
    case XK_KP_4:
        return KeyCode::digit_4;
    case XK_5:
    case XK_KP_5:
        return KeyCode::digit_5;
    case XK_6:
    case XK_KP_6:
        return KeyCode::digit_6;
    case XK_7:
    case XK_KP_7:
        return KeyCode::digit_7;
    case XK_8:
    case XK_KP_8:
        return KeyCode::digit_8;
    case XK_9:
    case XK_KP_9:
        return KeyCode::digit_9;
    case XK_w:
    case XK_W:
        return KeyCode::w;
    case XK_a:
    case XK_A:
        return KeyCode::a;
    case XK_s:
    case XK_S:
        return KeyCode::s;
    case XK_d:
    case XK_D:
        return KeyCode::d;
    case XK_q:
    case XK_Q:
        return KeyCode::q;
    case XK_e:
    case XK_E:
        return KeyCode::e;
    case XK_r:
    case XK_R:
        return KeyCode::r;
    case XK_m:
    case XK_M:
        return KeyCode::m;
    case XK_Shift_L:
        return KeyCode::left_shift;
    case XK_Control_L:
        return KeyCode::left_control;
    case XK_Alt_L:
        return KeyCode::left_alt;
    case XK_F1:
        return KeyCode::f1;
    case XK_F3:
        return KeyCode::f3;
    case XK_F4:
        return KeyCode::f4;
    case XK_F5:
        return KeyCode::f5;
    case XK_F7:
        return KeyCode::f7;
    default:
        return KeyCode::unknown;
    }
}

[[nodiscard]] MouseButton mouse_button_from_x11_button(unsigned int button) noexcept {
    switch (button) {
    case Button1:
        return MouseButton::left;
    case Button2:
        return MouseButton::middle;
    case Button3:
        return MouseButton::right;
    case 8:
        return MouseButton::x1;
    case 9:
        return MouseButton::x2;
    default:
        return MouseButton::unknown;
    }
}

[[nodiscard]] std::size_t x11_property_bytes_per_item(int format) noexcept {
    switch (format) {
    case 8:
        return 1;
    case 16:
        return sizeof(short);
    case 32:
        return sizeof(long);
    default:
        return 0;
    }
}

[[nodiscard]] std::size_t x11_property_wire_bytes(unsigned long item_count, int format) noexcept {
    switch (format) {
    case 8:
        return static_cast<std::size_t>(item_count);
    case 16:
        return static_cast<std::size_t>(item_count) * 2U;
    case 32:
        return static_cast<std::size_t>(item_count) * 4U;
    default:
        return 0;
    }
}

[[nodiscard]] unsigned long x11_property_item_count_from_bytes(std::size_t byte_count,
                                                               int format) noexcept {
    const auto bytes_per_item = x11_property_bytes_per_item(format);
    if (bytes_per_item == 0) {
        return 0;
    }
    return static_cast<unsigned long>(byte_count / bytes_per_item);
}

[[nodiscard]] DisplayInfo display_info_from_x11_screen(Display* display, int screen_index,
                                                       int primary_screen) {
    const auto* screen = ScreenOfDisplay(display, screen_index);
    DisplayInfo info;
    info.index = static_cast<std::uint32_t>(screen_index);
    info.name = "x11_screen_" + std::to_string(screen_index);
    info.primary = screen_index == primary_screen;
    if (screen == nullptr) {
        return info;
    }

    info.width_px = static_cast<std::uint32_t>(std::max(0, WidthOfScreen(screen)));
    info.height_px = static_cast<std::uint32_t>(std::max(0, HeightOfScreen(screen)));
    info.width_mm = static_cast<std::uint32_t>(std::max(0, WidthMMOfScreen(screen)));
    info.height_mm = static_cast<std::uint32_t>(std::max(0, HeightMMOfScreen(screen)));
    if (info.width_mm > 0) {
        info.dpi_x =
            (static_cast<double>(info.width_px) * 25.4) / static_cast<double>(info.width_mm);
    }
    if (info.height_mm > 0) {
        info.dpi_y =
            (static_cast<double>(info.height_px) * 25.4) / static_cast<double>(info.height_mm);
    }
    return info;
}

#if HEARTSTEAD_HAS_XRANDR

[[nodiscard]] std::uint32_t clamp_to_u32(unsigned long value) noexcept {
    constexpr auto max_u32 = std::numeric_limits<std::uint32_t>::max();
    return value > max_u32 ? max_u32 : static_cast<std::uint32_t>(value);
}

[[nodiscard]] double refresh_hz_from_xrandr_mode(const XRRScreenResources& resources, RRMode mode) {
    for (int index = 0; index < resources.nmode; ++index) {
        const auto& mode_info = resources.modes[index];
        if (mode_info.id != mode || mode_info.hTotal == 0 || mode_info.vTotal == 0) {
            continue;
        }

        return static_cast<double>(mode_info.dotClock) /
               (static_cast<double>(mode_info.hTotal) * static_cast<double>(mode_info.vTotal));
    }
    return 0.0;
}

[[nodiscard]] std::vector<DisplayInfo> display_infos_from_xrandr(Display* display,
                                                                 int screen_index) {
    int event_base = 0;
    int error_base = 0;
    if (XRRQueryExtension(display, &event_base, &error_base) == 0) {
        return {};
    }

    const auto root = RootWindow(display, screen_index);
    XRRScreenResources* resources = XRRGetScreenResourcesCurrent(display, root);
    if (resources == nullptr) {
        return {};
    }

    std::vector<DisplayInfo> result;
    const auto primary_output = XRRGetOutputPrimary(display, root);
    result.reserve(static_cast<std::size_t>(std::max(0, resources->noutput)));

    for (int output_index = 0; output_index < resources->noutput; ++output_index) {
        const auto output = resources->outputs[output_index];
        XRROutputInfo* output_info = XRRGetOutputInfo(display, resources, output);
        if (output_info == nullptr) {
            continue;
        }
        if (output_info->connection != RR_Connected || output_info->crtc == None) {
            XRRFreeOutputInfo(output_info);
            continue;
        }

        XRRCrtcInfo* crtc_info = XRRGetCrtcInfo(display, resources, output_info->crtc);
        if (crtc_info == nullptr) {
            XRRFreeOutputInfo(output_info);
            continue;
        }

        DisplayInfo info;
        info.index = static_cast<std::uint32_t>(result.size());
        info.name =
            output_info->nameLen > 0
                ? std::string(output_info->name,
                              output_info->name + static_cast<std::size_t>(output_info->nameLen))
                : "xrandr_output_" + std::to_string(output_index);
        info.x_px = crtc_info->x;
        info.y_px = crtc_info->y;
        info.width_px = crtc_info->width;
        info.height_px = crtc_info->height;
        info.width_mm = clamp_to_u32(output_info->mm_width);
        info.height_mm = clamp_to_u32(output_info->mm_height);
        if (info.width_mm > 0) {
            info.dpi_x =
                (static_cast<double>(info.width_px) * 25.4) / static_cast<double>(info.width_mm);
        }
        if (info.height_mm > 0) {
            info.dpi_y =
                (static_cast<double>(info.height_px) * 25.4) / static_cast<double>(info.height_mm);
        }
        info.refresh_hz = refresh_hz_from_xrandr_mode(*resources, crtc_info->mode);
        info.primary = output == primary_output;
        result.push_back(std::move(info));

        XRRFreeCrtcInfo(crtc_info);
        XRRFreeOutputInfo(output_info);
    }

    XRRFreeScreenResources(resources);
    if (!result.empty() &&
        std::ranges::none_of(result, [](const DisplayInfo& info) { return info.primary; })) {
        result.front().primary = true;
    }
    return result;
}

#endif

#endif

} // namespace

PlatformClock::PlatformClock() : start_ms_(monotonic_ms()) {}

std::int64_t PlatformClock::now_ms() const noexcept {
    return monotonic_ms();
}

std::int64_t PlatformClock::elapsed_ms() const noexcept {
    return now_ms() - start_ms_;
}

std::int64_t PlatformClock::wall_time_ms() noexcept {
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    return std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
}

PlatformBackend HeadlessPlatform::backend() const noexcept {
    return PlatformBackend::headless;
}

std::string_view HeadlessPlatform::backend_name() const noexcept {
    return platform_backend_name(PlatformBackend::headless);
}

PlatformBackendCapabilities HeadlessPlatform::capabilities() const noexcept {
    return platform_backend_capabilities(PlatformBackend::headless);
}

std::vector<DisplayInfo> HeadlessPlatform::displays() const {
    return {make_headless_display_info()};
}

core::Status HeadlessPlatform::set_clipboard_text(std::string text) {
    clipboard_text_ = std::move(text);
    return core::Status::ok();
}

core::Result<std::string> HeadlessPlatform::clipboard_text() const {
    return core::Result<std::string>::success(clipboard_text_);
}

core::Result<WindowId> HeadlessPlatform::create_window(const WindowDesc& desc) {
    auto status = validate_window_desc(desc);
    if (!status) {
        return core::Result<WindowId>::failure(status.error().code, status.error().message);
    }

    const auto id = next_window_id();
    windows_.emplace(id.value(), WindowState{id, desc.title, desc.width, desc.height,
                                             desc.resizable, true, desc.fullscreen});
    emit_event(PlatformEvent{
        PlatformEventKind::window_created, id, KeyCode::unknown, desc.width, desc.height, {}});
    return core::Result<WindowId>::success(id);
}

core::Status HeadlessPlatform::close_window(WindowId id) {
    const auto found = windows_.find(id.value());
    if (found == windows_.end() || !found->second.open) {
        return core::Status::failure("platform.window_not_open", "window is not open");
    }

    found->second.open = false;
    input_.erase(id.value());
    mouse_buttons_.erase(id.value());
    mouse_positions_.erase(id.value());
    mouse_delta_.erase(id.value());
    cursor_captured_.erase(id.value());
    mouse_wheel_.erase(id.value());
    text_input_.erase(id.value());
    emit_event(PlatformEvent{PlatformEventKind::window_closed, id, KeyCode::unknown, 0, 0, {}});
    return core::Status::ok();
}

const WindowState* HeadlessPlatform::find_window(WindowId id) const noexcept {
    const auto found = windows_.find(id.value());
    return found == windows_.end() ? nullptr : &found->second;
}

std::optional<NativeWindowHandle> HeadlessPlatform::native_window_handle(WindowId) const noexcept {
    return std::nullopt;
}

std::size_t HeadlessPlatform::open_window_count() const noexcept {
    std::size_t count = 0;
    for (const auto& [_, window] : windows_) {
        if (window.open) {
            ++count;
        }
    }
    return count;
}

void HeadlessPlatform::begin_frame() {
    for (auto& [_, window_input] : input_) {
        for (auto& [ignored_key_code, key] : window_input) {
            key.pressed = false;
            key.released = false;
        }
    }
    for (auto& [_, window_input] : mouse_buttons_) {
        for (auto& [ignored_button_code, button] : window_input) {
            button.pressed = false;
            button.released = false;
        }
    }
    mouse_wheel_.clear();
    mouse_delta_.clear();
    for (auto& [_, text] : text_input_) {
        text.clear();
    }
}

core::Status HeadlessPlatform::queue_event(PlatformEvent event) {
    auto status = validate_queued_event(event);
    if (!status) {
        return status;
    }

    if (event.kind == PlatformEventKind::window_resized) {
        auto& window = windows_.at(event.window_id.value());
        window.width = event.width;
        window.height = event.height;
    }
    if (event.kind == PlatformEventKind::window_closed) {
        auto& window = windows_.at(event.window_id.value());
        window.open = false;
        input_.erase(event.window_id.value());
        mouse_buttons_.erase(event.window_id.value());
        mouse_positions_.erase(event.window_id.value());
        mouse_delta_.erase(event.window_id.value());
        cursor_captured_.erase(event.window_id.value());
        mouse_wheel_.erase(event.window_id.value());
        text_input_.erase(event.window_id.value());
    }
    if (event.kind == PlatformEventKind::quit_requested) {
        quit_requested_ = true;
    }

    emit_event(std::move(event));
    return core::Status::ok();
}

std::optional<PlatformEvent> HeadlessPlatform::poll_event() {
    if (events_.empty()) {
        return std::nullopt;
    }

    auto event = std::move(events_.front());
    events_.pop();
    apply_input_event(event);
    return event;
}

std::optional<KeyInputState> HeadlessPlatform::key_state(WindowId window_id,
                                                         KeyCode key) const noexcept {
    const auto window = input_.find(window_id.value());
    if (window == input_.end()) {
        return std::nullopt;
    }

    const auto found = window->second.find(static_cast<std::uint32_t>(key));
    if (found == window->second.end()) {
        return std::nullopt;
    }
    return found->second;
}

std::optional<MouseButtonState>
HeadlessPlatform::mouse_button_state(WindowId window_id, MouseButton button) const noexcept {
    const auto window = mouse_buttons_.find(window_id.value());
    if (window == mouse_buttons_.end()) {
        return std::nullopt;
    }

    const auto found = window->second.find(static_cast<std::uint32_t>(button));
    if (found == window->second.end()) {
        return std::nullopt;
    }
    return found->second;
}

std::optional<WindowInputSnapshot>
HeadlessPlatform::input_snapshot(WindowId window_id) const noexcept {
    const auto window = windows_.find(window_id.value());
    if (window == windows_.end() || !window->second.open) {
        return std::nullopt;
    }

    WindowInputSnapshot snapshot;
    snapshot.window_id = window_id;

    const auto input = input_.find(window_id.value());
    if (input != input_.end()) {
        for (const auto& [key_value, state] : input->second) {
            const auto key = static_cast<KeyCode>(key_value);
            if (state.down) {
                snapshot.down_keys.push_back(key);
            }
            if (state.pressed) {
                snapshot.pressed_keys.push_back(key);
            }
            if (state.released) {
                snapshot.released_keys.push_back(key);
            }
        }
    }

    sort_keys(snapshot.down_keys);
    sort_keys(snapshot.pressed_keys);
    sort_keys(snapshot.released_keys);

    const auto mouse_buttons = mouse_buttons_.find(window_id.value());
    if (mouse_buttons != mouse_buttons_.end()) {
        for (const auto& [button_value, state] : mouse_buttons->second) {
            const auto button = static_cast<MouseButton>(button_value);
            if (state.down) {
                snapshot.down_mouse_buttons.push_back(button);
            }
            if (state.pressed) {
                snapshot.pressed_mouse_buttons.push_back(button);
            }
            if (state.released) {
                snapshot.released_mouse_buttons.push_back(button);
            }
        }
    }

    sort_mouse_buttons(snapshot.down_mouse_buttons);
    sort_mouse_buttons(snapshot.pressed_mouse_buttons);
    sort_mouse_buttons(snapshot.released_mouse_buttons);

    const auto mouse_position = mouse_positions_.find(window_id.value());
    if (mouse_position != mouse_positions_.end()) {
        snapshot.mouse = mouse_position->second;
    }

    const auto mouse_delta = mouse_delta_.find(window_id.value());
    if (mouse_delta != mouse_delta_.end()) {
        snapshot.mouse_delta_x = mouse_delta->second.first;
        snapshot.mouse_delta_y = mouse_delta->second.second;
    }

    const auto mouse_wheel = mouse_wheel_.find(window_id.value());
    if (mouse_wheel != mouse_wheel_.end()) {
        snapshot.wheel_delta_x = mouse_wheel->second.first;
        snapshot.wheel_delta_y = mouse_wheel->second.second;
    }

    const auto text = text_input_.find(window_id.value());
    if (text != text_input_.end()) {
        snapshot.text = text->second;
    }
    snapshot.cursor_captured = cursor_captured(window_id);
    return snapshot;
}

core::Status HeadlessPlatform::set_cursor_capture(WindowId window_id, bool captured) {
    if (!is_window_open(windows_, window_id)) {
        return core::Status::failure("platform.window_not_open", "window is not open");
    }
    cursor_captured_[window_id.value()] = captured;
    mouse_delta_.erase(window_id.value());
    return core::Status::ok();
}

bool HeadlessPlatform::cursor_captured(WindowId window_id) const noexcept {
    const auto found = cursor_captured_.find(window_id.value());
    return found != cursor_captured_.end() && found->second;
}

bool HeadlessPlatform::is_key_down(WindowId window_id, KeyCode key) const noexcept {
    const auto state = key_state(window_id, key);
    return state.has_value() && state->down;
}

bool HeadlessPlatform::was_key_pressed(WindowId window_id, KeyCode key) const noexcept {
    const auto state = key_state(window_id, key);
    return state.has_value() && state->pressed;
}

bool HeadlessPlatform::was_key_released(WindowId window_id, KeyCode key) const noexcept {
    const auto state = key_state(window_id, key);
    return state.has_value() && state->released;
}

bool HeadlessPlatform::is_mouse_button_down(WindowId window_id, MouseButton button) const noexcept {
    const auto state = mouse_button_state(window_id, button);
    return state.has_value() && state->down;
}

bool HeadlessPlatform::was_mouse_button_pressed(WindowId window_id,
                                                MouseButton button) const noexcept {
    const auto state = mouse_button_state(window_id, button);
    return state.has_value() && state->pressed;
}

bool HeadlessPlatform::was_mouse_button_released(WindowId window_id,
                                                 MouseButton button) const noexcept {
    const auto state = mouse_button_state(window_id, button);
    return state.has_value() && state->released;
}

void HeadlessPlatform::request_quit() {
    if (quit_requested_) {
        return;
    }
    quit_requested_ = true;
    emit_event(PlatformEvent{PlatformEventKind::quit_requested, {}, KeyCode::unknown, 0, 0, {}});
}

bool HeadlessPlatform::should_quit() const noexcept {
    return quit_requested_;
}

PlatformClock& HeadlessPlatform::clock() noexcept {
    return clock_;
}

const PlatformClock& HeadlessPlatform::clock() const noexcept {
    return clock_;
}

WindowId HeadlessPlatform::next_window_id() {
    const auto id = WindowId::from_value(next_window_id_);
    ++next_window_id_;
    return id;
}

void HeadlessPlatform::emit_event(PlatformEvent event) {
    events_.push(std::move(event));
}

core::Status HeadlessPlatform::validate_queued_event(const PlatformEvent& event) const {
    switch (event.kind) {
    case PlatformEventKind::quit_requested:
        return core::Status::ok();
    case PlatformEventKind::window_created:
        return core::Status::failure("platform.event_not_queueable",
                                     "window_created events are emitted by create_window");
    case PlatformEventKind::window_closed:
        if (!is_window_open(windows_, event.window_id)) {
            return core::Status::failure("platform.window_not_open", "window is not open");
        }
        return core::Status::ok();
    case PlatformEventKind::window_resized:
        if (!is_window_open(windows_, event.window_id)) {
            return core::Status::failure("platform.window_not_open", "window is not open");
        }
        if ((event.width == 0) != (event.height == 0)) {
            return core::Status::failure("platform.invalid_window_size",
                                         "window dimensions must both be zero or both be non-zero");
        }
        return core::Status::ok();
    case PlatformEventKind::window_focus_gained:
    case PlatformEventKind::window_focus_lost:
        if (!is_window_open(windows_, event.window_id)) {
            return core::Status::failure("platform.window_not_open", "window is not open");
        }
        return core::Status::ok();
    case PlatformEventKind::key_down:
    case PlatformEventKind::key_up:
        if (!is_window_open(windows_, event.window_id)) {
            return core::Status::failure("platform.window_not_open", "window is not open");
        }
        if (event.key == KeyCode::unknown) {
            return core::Status::failure("platform.invalid_key", "key event must name a key");
        }
        return core::Status::ok();
    case PlatformEventKind::text_input:
        if (!is_window_open(windows_, event.window_id)) {
            return core::Status::failure("platform.window_not_open", "window is not open");
        }
        if (event.text.empty()) {
            return core::Status::failure("platform.invalid_text_input",
                                         "text input event text must not be empty");
        }
        return core::Status::ok();
    case PlatformEventKind::mouse_moved:
        if (!is_window_open(windows_, event.window_id)) {
            return core::Status::failure("platform.window_not_open", "window is not open");
        }
        return core::Status::ok();
    case PlatformEventKind::mouse_button_down:
    case PlatformEventKind::mouse_button_up:
        if (!is_window_open(windows_, event.window_id)) {
            return core::Status::failure("platform.window_not_open", "window is not open");
        }
        if (event.mouse_button == MouseButton::unknown) {
            return core::Status::failure("platform.invalid_mouse_button",
                                         "mouse button event must name a button");
        }
        return core::Status::ok();
    case PlatformEventKind::mouse_wheel:
        if (!is_window_open(windows_, event.window_id)) {
            return core::Status::failure("platform.window_not_open", "window is not open");
        }
        if (event.wheel_delta_x == 0 && event.wheel_delta_y == 0) {
            return core::Status::failure("platform.invalid_mouse_wheel",
                                         "mouse wheel event must include a non-zero delta");
        }
        return core::Status::ok();
    }
    return core::Status::failure("platform.unknown_event", "unknown platform event kind");
}

void HeadlessPlatform::apply_input_event(const PlatformEvent& event) {
    if (event.kind == PlatformEventKind::window_focus_lost) {
        for (auto& [_, state] : input_[event.window_id.value()]) {
            state.released |= state.down;
            state.down = false;
        }
        for (auto& [_, state] : mouse_buttons_[event.window_id.value()]) {
            state.released |= state.down;
            state.down = false;
        }
        cursor_captured_[event.window_id.value()] = false;
        return;
    }
    if (event.kind == PlatformEventKind::text_input && !event.text.empty()) {
        text_input_[event.window_id.value()].push_back(event.text);
        return;
    }

    if (event.kind == PlatformEventKind::key_down) {
        auto& state = input_[event.window_id.value()][static_cast<std::uint32_t>(event.key)];
        state.pressed |= !state.down;
        state.down = true;
        return;
    }

    if (event.kind == PlatformEventKind::key_up) {
        auto& state = input_[event.window_id.value()][static_cast<std::uint32_t>(event.key)];
        state.released |= state.down;
        state.down = false;
        return;
    }

    if (event.kind == PlatformEventKind::mouse_moved ||
        event.kind == PlatformEventKind::mouse_button_down ||
        event.kind == PlatformEventKind::mouse_button_up ||
        event.kind == PlatformEventKind::mouse_wheel) {
        auto& position = mouse_positions_[event.window_id.value()];
        if (event.kind == PlatformEventKind::mouse_moved &&
            (event.mouse_delta_x != 0 || event.mouse_delta_y != 0)) {
            auto& delta = mouse_delta_[event.window_id.value()];
            delta.first += event.mouse_delta_x;
            delta.second += event.mouse_delta_y;
        } else if (event.kind == PlatformEventKind::mouse_moved && position.inside) {
            auto& delta = mouse_delta_[event.window_id.value()];
            delta.first += event.mouse_x - position.x;
            delta.second += event.mouse_y - position.y;
        }
        position = MousePosition{event.mouse_x, event.mouse_y, true};
    }

    if (event.kind == PlatformEventKind::mouse_button_down) {
        auto& state =
            mouse_buttons_[event.window_id.value()][static_cast<std::uint32_t>(event.mouse_button)];
        state.pressed |= !state.down;
        state.down = true;
        return;
    }

    if (event.kind == PlatformEventKind::mouse_button_up) {
        auto& state =
            mouse_buttons_[event.window_id.value()][static_cast<std::uint32_t>(event.mouse_button)];
        state.released |= state.down;
        state.down = false;
        return;
    }

    if (event.kind == PlatformEventKind::mouse_wheel) {
        auto& wheel = mouse_wheel_[event.window_id.value()];
        wheel.first += event.wheel_delta_x;
        wheel.second += event.wheel_delta_y;
    }
}

#if HEARTSTEAD_HAS_X11

namespace {

class X11NativePlatform final : public IPlatform {
  public:
    explicit X11NativePlatform(Display* display)
        : display_(display), screen_(DefaultScreen(display_)),
          wm_delete_window_(XInternAtom(display_, "WM_DELETE_WINDOW", False)),
          clipboard_atom_(XInternAtom(display_, "CLIPBOARD", False)),
          clipboard_targets_atom_(XInternAtom(display_, "TARGETS", False)),
          clipboard_utf8_string_atom_(XInternAtom(display_, "UTF8_STRING", False)),
          clipboard_text_atom_(XInternAtom(display_, "TEXT", False)),
          clipboard_string_atom_(XInternAtom(display_, "STRING", False)),
          clipboard_incr_atom_(XInternAtom(display_, "INCR", False)),
          clipboard_transfer_property_atom_(
              XInternAtom(display_, "HEARTSTEAD_CLIPBOARD_TRANSFER", False)) {
        clipboard_owner_window_ =
            XCreateSimpleWindow(display_, RootWindow(display_, screen_), -1, -1, 1, 1, 0, 0, 0);
        if (clipboard_owner_window_ != 0) {
            XSelectInput(display_, clipboard_owner_window_, PropertyChangeMask);
        }
        static const char invisible_cursor_bits[] = {0};
        const auto cursor_bitmap = XCreateBitmapFromData(display_, RootWindow(display_, screen_),
                                                         invisible_cursor_bits, 1, 1);
        if (cursor_bitmap != None) {
            XColor transparent{};
            invisible_cursor_ = XCreatePixmapCursor(display_, cursor_bitmap, cursor_bitmap,
                                                    &transparent, &transparent, 0, 0);
            XFreePixmap(display_, cursor_bitmap);
        }
    }

    ~X11NativePlatform() override {
        XUngrabPointer(display_, CurrentTime);
        for (const auto& [_, native_window] : native_windows_) {
            XUndefineCursor(display_, native_window);
        }
        if (invisible_cursor_ != None) {
            XFreeCursor(display_, invisible_cursor_);
        }
        if (clipboard_owner_window_ != 0) {
            if (XGetSelectionOwner(display_, clipboard_atom_) == clipboard_owner_window_) {
                XSetSelectionOwner(display_, clipboard_atom_, None, CurrentTime);
            }
            XDestroyWindow(display_, clipboard_owner_window_);
        }
        for (const auto& [_, native_window] : native_windows_) {
            XDestroyWindow(display_, native_window);
        }
        XFlush(display_);
        XCloseDisplay(display_);
    }

    X11NativePlatform(const X11NativePlatform&) = delete;
    X11NativePlatform& operator=(const X11NativePlatform&) = delete;
    X11NativePlatform(X11NativePlatform&&) = delete;
    X11NativePlatform& operator=(X11NativePlatform&&) = delete;

    [[nodiscard]] PlatformBackend backend() const noexcept override {
        return PlatformBackend::native;
    }

    [[nodiscard]] std::string_view backend_name() const noexcept override {
        return platform_backend_name(PlatformBackend::native);
    }

    [[nodiscard]] PlatformBackendCapabilities capabilities() const noexcept override {
        return platform_backend_capabilities(PlatformBackend::native);
    }

    [[nodiscard]] std::vector<DisplayInfo> displays() const override {
#if HEARTSTEAD_HAS_XRANDR
        auto xrandr_displays = display_infos_from_xrandr(display_, screen_);
        if (!xrandr_displays.empty()) {
            return xrandr_displays;
        }
#endif

        std::vector<DisplayInfo> result;
        const auto screen_count = ScreenCount(display_);
        if (screen_count <= 0) {
            return result;
        }

        result.reserve(static_cast<std::size_t>(screen_count));
        for (int index = 0; index < screen_count; ++index) {
            result.push_back(display_info_from_x11_screen(display_, index, screen_));
        }
        return result;
    }

    [[nodiscard]] core::Status set_clipboard_text(std::string text) override {
        if (clipboard_owner_window_ == 0) {
            return core::Status::failure("platform.clipboard_unavailable",
                                         "X11 clipboard owner window is not available");
        }

        clipboard_text_ = std::move(text);
        owns_clipboard_selection_ = true;
        XSetSelectionOwner(display_, clipboard_atom_, clipboard_owner_window_, CurrentTime);
        XFlush(display_);

        if (XGetSelectionOwner(display_, clipboard_atom_) != clipboard_owner_window_) {
            owns_clipboard_selection_ = false;
            return core::Status::failure("platform.clipboard_unavailable",
                                         "failed to take ownership of the X11 clipboard");
        }
        return core::Status::ok();
    }

    [[nodiscard]] core::Result<std::string> clipboard_text() const override {
        if (clipboard_owner_window_ == 0) {
            return core::Result<std::string>::failure(
                "platform.clipboard_unavailable", "X11 clipboard owner window is not available");
        }

        const auto owner = XGetSelectionOwner(display_, clipboard_atom_);
        if (owner == clipboard_owner_window_ && owns_clipboard_selection_) {
            return core::Result<std::string>::success(clipboard_text_);
        }
        if (owner == None) {
            return core::Result<std::string>::success({});
        }
        return request_external_clipboard_text();
    }

    [[nodiscard]] core::Result<WindowId> create_window(const WindowDesc& desc) override {
        auto status = validate_window_desc(desc);
        if (!status) {
            return core::Result<WindowId>::failure(status.error().code, status.error().message);
        }

        auto id = logical_.create_window(desc);
        if (!id) {
            return id;
        }

        const auto root = RootWindow(display_, screen_);
        const auto native_window =
            XCreateSimpleWindow(display_, root, 0, 0, desc.width, desc.height, 0,
                                BlackPixel(display_, screen_), BlackPixel(display_, screen_));
        if (native_window == 0) {
            (void)logical_.close_window(id.value());
            return core::Result<WindowId>::failure("platform.native_window_failed",
                                                   "failed to create X11 window");
        }

        XStoreName(display_, native_window, desc.title.c_str());
        XSelectInput(display_, native_window,
                     StructureNotifyMask | KeyPressMask | KeyReleaseMask | ButtonPressMask |
                         ButtonReleaseMask | PointerMotionMask | ExposureMask | FocusChangeMask);
        XSetWMProtocols(display_, native_window, &wm_delete_window_, 1);

        if (desc.fullscreen) {
            const auto wm_state = XInternAtom(display_, "_NET_WM_STATE", False);
            const auto fullscreen = XInternAtom(display_, "_NET_WM_STATE_FULLSCREEN", False);
            XChangeProperty(display_, native_window, wm_state, XA_ATOM, 32, PropModeReplace,
                            reinterpret_cast<const unsigned char*>(&fullscreen), 1);
        }

        if (!desc.resizable) {
            XSizeHints size_hints{};
            size_hints.flags = PMinSize | PMaxSize;
            size_hints.min_width = static_cast<int>(desc.width);
            size_hints.min_height = static_cast<int>(desc.height);
            size_hints.max_width = static_cast<int>(desc.width);
            size_hints.max_height = static_cast<int>(desc.height);
            XSetWMNormalHints(display_, native_window, &size_hints);
        }

        native_windows_.emplace(id.value().value(), native_window);
        window_ids_by_native_.emplace(native_window, id.value().value());
        XMapWindow(display_, native_window);
        XFlush(display_);
        return id;
    }

    [[nodiscard]] core::Status close_window(WindowId id) override {
        const auto found = native_windows_.find(id.value());
        if (found != native_windows_.end()) {
            cursor_capture_requests_.erase(id.value());
            unfocused_windows_.erase(id.value());
            if (logical_.cursor_captured(id)) {
                XUngrabPointer(display_, CurrentTime);
            }
            XUndefineCursor(display_, found->second);
            window_ids_by_native_.erase(found->second);
            XDestroyWindow(display_, found->second);
            XFlush(display_);
            native_windows_.erase(found);
        }
        return logical_.close_window(id);
    }

    [[nodiscard]] const WindowState* find_window(WindowId id) const noexcept override {
        return logical_.find_window(id);
    }

    [[nodiscard]] std::optional<NativeWindowHandle>
    native_window_handle(WindowId id) const noexcept override {
        const auto found = native_windows_.find(id.value());
        if (found == native_windows_.end()) {
            return std::nullopt;
        }
        return NativeWindowHandle{
            NativeWindowSystem::x11,
            display_,
            static_cast<std::uint64_t>(found->second),
        };
    }

    [[nodiscard]] std::size_t open_window_count() const noexcept override {
        return logical_.open_window_count();
    }

    void begin_frame() override {
        logical_.begin_frame();
        pump_x11_events();
        retry_pending_cursor_captures();
    }

    [[nodiscard]] core::Status queue_event(PlatformEvent event) override {
        if (event.kind == PlatformEventKind::window_closed) {
            return close_window(event.window_id);
        }
        if (event.kind == PlatformEventKind::window_resized) {
            const auto found = native_windows_.find(event.window_id.value());
            if (found != native_windows_.end() && event.width > 0 && event.height > 0) {
                XResizeWindow(display_, found->second, event.width, event.height);
                XFlush(display_);
            }
        }
        return logical_.queue_event(std::move(event));
    }

    [[nodiscard]] std::optional<PlatformEvent> poll_event() override {
        return logical_.poll_event();
    }

    [[nodiscard]] std::optional<KeyInputState> key_state(WindowId window_id,
                                                         KeyCode key) const noexcept override {
        return logical_.key_state(window_id, key);
    }

    [[nodiscard]] std::optional<MouseButtonState>
    mouse_button_state(WindowId window_id, MouseButton button) const noexcept override {
        return logical_.mouse_button_state(window_id, button);
    }

    [[nodiscard]] std::optional<WindowInputSnapshot>
    input_snapshot(WindowId window_id) const noexcept override {
        return logical_.input_snapshot(window_id);
    }

    [[nodiscard]] core::Status set_cursor_capture(WindowId window_id, bool captured) override {
        const auto found = native_windows_.find(window_id.value());
        if (found == native_windows_.end()) {
            return core::Status::failure("platform.window_not_open", "window is not open");
        }
        if (captured) {
            cursor_capture_requests_.insert(window_id.value());
            if (unfocused_windows_.contains(window_id.value())) {
                return core::Status::ok();
            }
            const auto result = try_capture_cursor(window_id, found->second);
            if (result == GrabSuccess || x11_pointer_grab_result_is_transient(result)) {
                return core::Status::ok();
            }
            cursor_capture_requests_.erase(window_id.value());
            return core::Status::failure("platform.cursor_capture_failed",
                                         "X11 pointer grab failed with " +
                                             std::string(x11_pointer_grab_result_name(result)));
        } else {
            cursor_capture_requests_.erase(window_id.value());
            XUngrabPointer(display_, CurrentTime);
            XUndefineCursor(display_, found->second);
            XFlush(display_);
            return logical_.set_cursor_capture(window_id, false);
        }
    }

    [[nodiscard]] bool cursor_captured(WindowId window_id) const noexcept override {
        return logical_.cursor_captured(window_id);
    }

    [[nodiscard]] bool is_key_down(WindowId window_id, KeyCode key) const noexcept override {
        return logical_.is_key_down(window_id, key);
    }

    [[nodiscard]] bool was_key_pressed(WindowId window_id, KeyCode key) const noexcept override {
        return logical_.was_key_pressed(window_id, key);
    }

    [[nodiscard]] bool was_key_released(WindowId window_id, KeyCode key) const noexcept override {
        return logical_.was_key_released(window_id, key);
    }

    [[nodiscard]] bool is_mouse_button_down(WindowId window_id,
                                            MouseButton button) const noexcept override {
        return logical_.is_mouse_button_down(window_id, button);
    }

    [[nodiscard]] bool was_mouse_button_pressed(WindowId window_id,
                                                MouseButton button) const noexcept override {
        return logical_.was_mouse_button_pressed(window_id, button);
    }

    [[nodiscard]] bool was_mouse_button_released(WindowId window_id,
                                                 MouseButton button) const noexcept override {
        return logical_.was_mouse_button_released(window_id, button);
    }

    void request_quit() override {
        logical_.request_quit();
    }

    [[nodiscard]] bool should_quit() const noexcept override {
        return logical_.should_quit();
    }

    [[nodiscard]] PlatformClock& clock() noexcept override {
        return logical_.clock();
    }

    [[nodiscard]] const PlatformClock& clock() const noexcept override {
        return logical_.clock();
    }

  private:
    [[nodiscard]] int try_capture_cursor(WindowId id, ::Window native_window) {
        if (logical_.cursor_captured(id)) {
            return GrabSuccess;
        }
        const auto result = XGrabPointer(
            display_, native_window, True, PointerMotionMask | ButtonPressMask | ButtonReleaseMask,
            GrabModeAsync, GrabModeAsync, native_window, invisible_cursor_, CurrentTime);
        if (result != GrabSuccess) {
            return result;
        }
        if (invisible_cursor_ != None) {
            XDefineCursor(display_, native_window, invisible_cursor_);
        }
        const auto* state = logical_.find_window(id);
        if (state != nullptr) {
            XWarpPointer(display_, None, native_window, 0, 0, 0, 0,
                         static_cast<int>(state->width / 2), static_cast<int>(state->height / 2));
        }
        XFlush(display_);
        const auto status = logical_.set_cursor_capture(id, true);
        return status ? GrabSuccess : GrabInvalidTime;
    }

    void retry_pending_cursor_captures() {
        for (const auto id_value : cursor_capture_requests_) {
            if (unfocused_windows_.contains(id_value)) {
                continue;
            }
            const auto found = native_windows_.find(id_value);
            if (found == native_windows_.end()) {
                continue;
            }
            (void)try_capture_cursor(WindowId::from_value(id_value), found->second);
        }
    }

    [[nodiscard]] std::optional<WindowId> window_id_for(::Window native_window) const noexcept {
        const auto found = window_ids_by_native_.find(native_window);
        if (found == window_ids_by_native_.end()) {
            return std::nullopt;
        }
        return WindowId::from_value(found->second);
    }

    void pump_x11_events() {
        while (XPending(display_) > 0) {
            XEvent event{};
            XNextEvent(display_, &event);

            switch (event.type) {
            case ConfigureNotify:
                handle_configure(event.xconfigure);
                break;
            case MapNotify:
                handle_map(event.xmap);
                break;
            case UnmapNotify:
                handle_unmap(event.xunmap);
                break;
            case FocusIn:
                handle_focus(event.xfocus, true);
                break;
            case FocusOut:
                handle_focus(event.xfocus, false);
                break;
            case KeyPress:
                handle_key_press(event.xkey);
                break;
            case KeyRelease:
                handle_key_release(event.xkey);
                break;
            case MotionNotify:
                handle_motion(event.xmotion);
                break;
            case ButtonPress:
                handle_button_press(event.xbutton);
                break;
            case ButtonRelease:
                handle_button_release(event.xbutton);
                break;
            case ClientMessage:
                handle_client_message(event.xclient);
                break;
            case SelectionClear:
                handle_selection_clear(event.xselectionclear);
                break;
            case SelectionRequest:
                handle_selection_request(event.xselectionrequest);
                break;
            case PropertyNotify:
                handle_property_notify(event.xproperty);
                break;
            case DestroyNotify:
                handle_destroy(event.xdestroywindow);
                break;
            default:
                break;
            }
        }
    }

    void queue_window_resize(WindowId id, std::uint32_t width, std::uint32_t height) {
        const auto* state = logical_.find_window(id);
        if (state != nullptr && state->width == width && state->height == height) {
            return;
        }
        (void)logical_.queue_event(PlatformEvent{
            PlatformEventKind::window_resized, id, KeyCode::unknown, width, height, {}});
    }

    void handle_configure(const XConfigureEvent& event) {
        auto id = window_id_for(event.window);
        if (!id || event.width <= 0 || event.height <= 0) {
            return;
        }
        queue_window_resize(id.value(), static_cast<std::uint32_t>(event.width),
                            static_cast<std::uint32_t>(event.height));
    }

    void handle_map(const XMapEvent& event) {
        auto id = window_id_for(event.window);
        if (!id) {
            return;
        }
        XWindowAttributes attributes{};
        if (XGetWindowAttributes(display_, event.window, &attributes) == 0 ||
            attributes.width <= 0 || attributes.height <= 0) {
            return;
        }
        queue_window_resize(id.value(), static_cast<std::uint32_t>(attributes.width),
                            static_cast<std::uint32_t>(attributes.height));
    }

    void handle_unmap(const XUnmapEvent& event) {
        if (event.from_configure != False) {
            return;
        }
        auto id = window_id_for(event.window);
        if (id) {
            queue_window_resize(id.value(), 0, 0);
        }
    }

    void handle_focus(const XFocusChangeEvent& event, bool focused) {
        auto id = window_id_for(event.window);
        if (!id) {
            return;
        }
        if (focused) {
            unfocused_windows_.erase(id->value());
        } else {
            unfocused_windows_.insert(id->value());
        }
        (void)logical_.queue_event(PlatformEvent{focused ? PlatformEventKind::window_focus_gained
                                                         : PlatformEventKind::window_focus_lost,
                                                 id.value(),
                                                 KeyCode::unknown,
                                                 0,
                                                 0,
                                                 {}});
        if (!focused) {
            XUngrabPointer(display_, CurrentTime);
            XUndefineCursor(display_, event.window);
            XFlush(display_);
        }
    }

    void handle_key_press(XKeyEvent& event) {
        auto id = window_id_for(event.window);
        if (!id) {
            return;
        }

        KeySym key_symbol = NoSymbol;
        char text_buffer[32]{};
        const auto text_length =
            XLookupString(&event, text_buffer, sizeof(text_buffer), &key_symbol, nullptr);
        const auto key = key_from_x11_keysym(key_symbol);
        if (key != KeyCode::unknown) {
            (void)logical_.queue_event(
                PlatformEvent{PlatformEventKind::key_down, id.value(), key, 0, 0, {}});
        }
        if (text_length > 0) {
            (void)logical_.queue_event(PlatformEvent{
                PlatformEventKind::text_input,
                id.value(),
                KeyCode::unknown,
                0,
                0,
                std::string(text_buffer, static_cast<std::size_t>(text_length)),
            });
        }
    }

    void handle_key_release(XKeyEvent& event) {
        auto id = window_id_for(event.window);
        if (!id) {
            return;
        }
        const auto key = key_from_x11_keysym(XLookupKeysym(&event, 0));
        if (key != KeyCode::unknown) {
            (void)logical_.queue_event(
                PlatformEvent{PlatformEventKind::key_up, id.value(), key, 0, 0, {}});
        }
    }

    void handle_motion(const XMotionEvent& event) {
        auto id = window_id_for(event.window);
        if (!id) {
            return;
        }
        const auto* state = logical_.find_window(id.value());
        if (logical_.cursor_captured(id.value()) && state != nullptr) {
            const auto center_x = static_cast<std::int32_t>(state->width / 2);
            const auto center_y = static_cast<std::int32_t>(state->height / 2);
            if (event.x == center_x && event.y == center_y) {
                return;
            }
            PlatformEvent relative;
            relative.kind = PlatformEventKind::mouse_moved;
            relative.window_id = id.value();
            relative.mouse_x = center_x;
            relative.mouse_y = center_y;
            relative.mouse_delta_x = static_cast<std::int32_t>(event.x) - center_x;
            relative.mouse_delta_y = static_cast<std::int32_t>(event.y) - center_y;
            (void)logical_.queue_event(std::move(relative));
            XWarpPointer(display_, None, event.window, 0, 0, 0, 0, center_x, center_y);
            XFlush(display_);
            return;
        }
        (void)logical_.queue_event(PlatformEvent{
            PlatformEventKind::mouse_moved,
            id.value(),
            KeyCode::unknown,
            0,
            0,
            {},
            MouseButton::unknown,
            static_cast<std::int32_t>(event.x),
            static_cast<std::int32_t>(event.y),
            0,
            0,
        });
    }

    void handle_button_press(const XButtonEvent& event) {
        auto id = window_id_for(event.window);
        if (!id) {
            return;
        }

        switch (event.button) {
        case Button4:
            queue_wheel_event(id.value(), event.x, event.y, 0, 1);
            return;
        case Button5:
            queue_wheel_event(id.value(), event.x, event.y, 0, -1);
            return;
        case 6:
            queue_wheel_event(id.value(), event.x, event.y, -1, 0);
            return;
        case 7:
            queue_wheel_event(id.value(), event.x, event.y, 1, 0);
            return;
        default:
            break;
        }

        const auto button = mouse_button_from_x11_button(event.button);
        if (button == MouseButton::unknown) {
            return;
        }

        (void)logical_.queue_event(PlatformEvent{
            PlatformEventKind::mouse_button_down,
            id.value(),
            KeyCode::unknown,
            0,
            0,
            {},
            button,
            static_cast<std::int32_t>(event.x),
            static_cast<std::int32_t>(event.y),
            0,
            0,
        });
    }

    void handle_button_release(const XButtonEvent& event) {
        auto id = window_id_for(event.window);
        if (!id || event.button == Button4 || event.button == Button5 || event.button == 6 ||
            event.button == 7) {
            return;
        }

        const auto button = mouse_button_from_x11_button(event.button);
        if (button == MouseButton::unknown) {
            return;
        }

        (void)logical_.queue_event(PlatformEvent{
            PlatformEventKind::mouse_button_up,
            id.value(),
            KeyCode::unknown,
            0,
            0,
            {},
            button,
            static_cast<std::int32_t>(event.x),
            static_cast<std::int32_t>(event.y),
            0,
            0,
        });
    }

    void queue_wheel_event(WindowId id, int x, int y, std::int32_t delta_x, std::int32_t delta_y) {
        (void)logical_.queue_event(PlatformEvent{
            PlatformEventKind::mouse_wheel,
            id,
            KeyCode::unknown,
            0,
            0,
            {},
            MouseButton::unknown,
            static_cast<std::int32_t>(x),
            static_cast<std::int32_t>(y),
            delta_x,
            delta_y,
        });
    }

    void handle_client_message(const XClientMessageEvent& event) {
        if (static_cast<Atom>(event.data.l[0]) != wm_delete_window_) {
            return;
        }
        auto id = window_id_for(event.window);
        if (!id) {
            return;
        }
        // WM_DELETE_WINDOW is a close request, not permission to invalidate graphics surfaces
        // immediately. Keep the native window alive until the application has torn down Vulkan,
        // then let its normal close_window() path destroy the X11 window.
        (void)logical_.queue_event(PlatformEvent{
            PlatformEventKind::quit_requested, id.value(), KeyCode::unknown, 0, 0, {}});
        request_quit();
    }

    void handle_destroy(const XDestroyWindowEvent& event) {
        auto id = window_id_for(event.window);
        if (!id) {
            return;
        }
        cursor_capture_requests_.erase(id->value());
        unfocused_windows_.erase(id->value());
        native_windows_.erase(id->value());
        window_ids_by_native_.erase(event.window);
        if (const auto* state = logical_.find_window(id.value()); state != nullptr && state->open) {
            (void)logical_.queue_event(PlatformEvent{
                PlatformEventKind::window_closed, id.value(), KeyCode::unknown, 0, 0, {}});
        }
    }

    [[nodiscard]] const char* clipboard_target_name(Atom target) const noexcept {
        if (target == clipboard_targets_atom_) {
            return "TARGETS";
        }
        if (target == clipboard_utf8_string_atom_) {
            return "UTF8_STRING";
        }
        if (target == clipboard_string_atom_) {
            return "STRING";
        }
        if (target == clipboard_text_atom_) {
            return "TEXT";
        }
        if (target == clipboard_incr_atom_) {
            return "INCR";
        }
        return "unknown";
    }

    [[nodiscard]] bool clipboard_target_available(const std::vector<Atom>& targets,
                                                  Atom target) const noexcept {
        return std::ranges::find(targets, target) != targets.end();
    }

    [[nodiscard]] core::Result<X11SelectionProperty>
    read_selection_property(Atom property, bool delete_property = true) const {
        X11SelectionProperty selection_property;
        long offset_32bit_units = 0;

        for (;;) {
            Atom actual_type = None;
            int actual_format = 0;
            unsigned long item_count = 0;
            unsigned long bytes_after = 0;
            unsigned char* data = nullptr;

            const auto result =
                XGetWindowProperty(display_, clipboard_owner_window_, property, offset_32bit_units,
                                   max_property_read_32bit_units, False, AnyPropertyType,
                                   &actual_type, &actual_format, &item_count, &bytes_after, &data);
            if (result != Success) {
                if (data != nullptr) {
                    XFree(data);
                }
                return core::Result<X11SelectionProperty>::failure(
                    "platform.clipboard_read_failed",
                    "failed to read X11 clipboard selection data");
            }

            const auto bytes_per_item = x11_property_bytes_per_item(actual_format);
            if ((actual_format != 0 && bytes_per_item == 0) ||
                (bytes_per_item > 0 &&
                 static_cast<std::size_t>(item_count) >
                     std::numeric_limits<std::size_t>::max() / bytes_per_item)) {
                if (data != nullptr) {
                    XFree(data);
                }
                return core::Result<X11SelectionProperty>::failure(
                    "platform.clipboard_unsupported_format",
                    "X11 clipboard selection data used an unsupported property format");
            }

            const auto byte_count = static_cast<std::size_t>(item_count) * bytes_per_item;
            if (selection_property.type == None) {
                selection_property.type = actual_type;
                selection_property.format = actual_format;
            } else if (actual_type != selection_property.type ||
                       actual_format != selection_property.format) {
                if (data != nullptr) {
                    XFree(data);
                }
                return core::Result<X11SelectionProperty>::failure(
                    "platform.clipboard_unsupported_format",
                    "X11 clipboard selection data changed type while reading");
            }

            if (byte_count > max_clipboard_transfer_bytes - selection_property.bytes.size()) {
                if (data != nullptr) {
                    XFree(data);
                }
                return core::Result<X11SelectionProperty>::failure(
                    "platform.clipboard_too_large",
                    "X11 clipboard selection data exceeded the platform transfer limit");
            }

            if (data != nullptr && byte_count > 0) {
                selection_property.bytes.insert(selection_property.bytes.end(), data,
                                                data + byte_count);
            }
            if (data != nullptr) {
                XFree(data);
            }

            const auto wire_bytes = x11_property_wire_bytes(item_count, actual_format);
            if (bytes_after > 0 && wire_bytes == 0) {
                return core::Result<X11SelectionProperty>::failure(
                    "platform.clipboard_read_failed",
                    "X11 clipboard property did not advance while reading");
            }
            offset_32bit_units += static_cast<long>((wire_bytes + 3U) / 4U);
            if (bytes_after == 0) {
                break;
            }
        }

        selection_property.item_count = x11_property_item_count_from_bytes(
            selection_property.bytes.size(), selection_property.format);
        if (delete_property) {
            XDeleteProperty(display_, clipboard_owner_window_, property);
            XFlush(display_);
        }
        return core::Result<X11SelectionProperty>::success(std::move(selection_property));
    }

    [[nodiscard]] core::Result<X11SelectionProperty>
    read_incremental_selection_property(Atom property) const {
        X11SelectionProperty completed_property;
        auto deadline = monotonic_ms() + clipboard_incremental_timeout_ms;

        while (monotonic_ms() <= deadline) {
            XEvent event{};
            if (!XCheckTypedWindowEvent(display_, clipboard_owner_window_, PropertyNotify,
                                        &event)) {
                XFlush(display_);
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }

            const auto& property_event = event.xproperty;
            if (property_event.atom != property || property_event.state != PropertyNewValue) {
                continue;
            }

            auto chunk = read_selection_property(property);
            if (!chunk) {
                return core::Result<X11SelectionProperty>::failure(chunk.error().code,
                                                                   chunk.error().message);
            }
            if (chunk.value().bytes.empty() && chunk.value().item_count == 0) {
                completed_property.item_count = x11_property_item_count_from_bytes(
                    completed_property.bytes.size(), completed_property.format);
                return core::Result<X11SelectionProperty>::success(std::move(completed_property));
            }

            if (completed_property.type == None) {
                completed_property.type = chunk.value().type;
                completed_property.format = chunk.value().format;
            } else if (completed_property.type != chunk.value().type ||
                       completed_property.format != chunk.value().format) {
                return core::Result<X11SelectionProperty>::failure(
                    "platform.clipboard_unsupported_format",
                    "X11 incremental clipboard chunks changed type while reading");
            }

            if (chunk.value().bytes.size() >
                max_clipboard_transfer_bytes - completed_property.bytes.size()) {
                return core::Result<X11SelectionProperty>::failure(
                    "platform.clipboard_too_large",
                    "X11 incremental clipboard data exceeded the platform transfer limit");
            }

            completed_property.bytes.insert(completed_property.bytes.end(),
                                            chunk.value().bytes.begin(), chunk.value().bytes.end());
            deadline = monotonic_ms() + clipboard_incremental_timeout_ms;
        }

        return core::Result<X11SelectionProperty>::failure(
            "platform.clipboard_timeout", "timed out waiting for X11 incremental clipboard data");
    }

    [[nodiscard]] core::Result<X11SelectionProperty> request_selection_property(Atom target) const {
        XDeleteProperty(display_, clipboard_owner_window_, clipboard_transfer_property_atom_);
        XConvertSelection(display_, clipboard_atom_, target, clipboard_transfer_property_atom_,
                          clipboard_owner_window_, CurrentTime);
        XFlush(display_);

        constexpr std::int64_t timeout_ms = 250;
        const auto deadline = monotonic_ms() + timeout_ms;
        while (monotonic_ms() <= deadline) {
            XEvent event{};
            if (XCheckTypedWindowEvent(display_, clipboard_owner_window_, SelectionNotify,
                                       &event)) {
                const auto& selection = event.xselection;
                if (selection.selection != clipboard_atom_ || selection.target != target) {
                    continue;
                }
                if (selection.property == None) {
                    return core::Result<X11SelectionProperty>::failure(
                        "platform.clipboard_target_unavailable",
                        "X11 clipboard owner did not provide target " +
                            std::string(clipboard_target_name(target)));
                }
                auto property = read_selection_property(selection.property);
                if (!property) {
                    return property;
                }
                if (property.value().type == clipboard_incr_atom_) {
                    return read_incremental_selection_property(selection.property);
                }
                return property;
            }

            XFlush(display_);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }

        return core::Result<X11SelectionProperty>::failure(
            "platform.clipboard_timeout", "timed out waiting for X11 clipboard target " +
                                              std::string(clipboard_target_name(target)));
    }

    [[nodiscard]] std::vector<Atom> request_external_clipboard_targets() const {
        auto targets_property = request_selection_property(clipboard_targets_atom_);
        if (!targets_property || targets_property.value().type != XA_ATOM ||
            targets_property.value().format != 32) {
            return {};
        }

        std::vector<Atom> targets;
        const auto atom_count =
            std::min(static_cast<std::size_t>(targets_property.value().item_count),
                     targets_property.value().bytes.size() / sizeof(Atom));
        targets.reserve(atom_count);
        for (std::size_t index = 0; index < atom_count; ++index) {
            const auto* first = targets_property.value().bytes.data() + (index * sizeof(Atom));
            Atom target = None;
            std::copy(first, first + sizeof(Atom), reinterpret_cast<unsigned char*>(&target));
            targets.push_back(target);
        }
        return targets;
    }

    [[nodiscard]] core::Result<std::string> request_external_clipboard_text(Atom target) const {
        auto property = request_selection_property(target);
        if (!property) {
            return core::Result<std::string>::failure(property.error().code,
                                                      property.error().message);
        }
        if (property.value().format != 8) {
            return core::Result<std::string>::failure(
                "platform.clipboard_unsupported_format",
                "X11 clipboard text target " + std::string(clipboard_target_name(target)) +
                    " did not return byte text");
        }
        if (target == clipboard_utf8_string_atom_ &&
            property.value().type != clipboard_utf8_string_atom_) {
            return core::Result<std::string>::failure(
                "platform.clipboard_unsupported_format",
                "X11 clipboard UTF8_STRING request returned an unexpected property type");
        }
        if ((target == clipboard_string_atom_ || target == clipboard_text_atom_) &&
            property.value().type != clipboard_string_atom_ &&
            property.value().type != clipboard_text_atom_ &&
            property.value().type != clipboard_utf8_string_atom_) {
            return core::Result<std::string>::failure(
                "platform.clipboard_unsupported_format",
                "X11 clipboard text request returned an unexpected property type");
        }

        return core::Result<std::string>::success(
            std::string(property.value().bytes.begin(), property.value().bytes.end()));
    }

    [[nodiscard]] core::Result<std::string> request_external_clipboard_text() const {
        std::vector<Atom> candidates;
        const auto targets = request_external_clipboard_targets();
        const std::array<Atom, 3> preferred_targets{
            clipboard_utf8_string_atom_,
            clipboard_string_atom_,
            clipboard_text_atom_,
        };

        if (targets.empty()) {
            candidates.assign(preferred_targets.begin(), preferred_targets.end());
        } else {
            for (const auto target : preferred_targets) {
                if (clipboard_target_available(targets, target)) {
                    candidates.push_back(target);
                }
            }
        }

        std::string last_error_code = "platform.clipboard_target_unavailable";
        std::string last_error_message =
            "X11 clipboard owner did not advertise a supported text target";
        for (const auto target : candidates) {
            auto text = request_external_clipboard_text(target);
            if (text) {
                return text;
            }
            last_error_code = text.error().code;
            last_error_message = text.error().message;
        }

        return core::Result<std::string>::failure(std::move(last_error_code),
                                                  std::move(last_error_message));
    }

    void handle_selection_clear(const XSelectionClearEvent& event) noexcept {
        if (event.selection == clipboard_atom_ && event.window == clipboard_owner_window_) {
            owns_clipboard_selection_ = false;
            pending_clipboard_transfers_.clear();
        }
    }

    void handle_selection_request(const XSelectionRequestEvent& request) {
        XSelectionEvent response{};
        response.type = SelectionNotify;
        response.display = request.display;
        response.requestor = request.requestor;
        response.selection = request.selection;
        response.target = request.target;
        response.time = request.time;
        response.property = None;

        if (request.selection == clipboard_atom_ && request.requestor != 0 &&
            owns_clipboard_selection_) {
            const auto property = request.property == None ? request.target : request.property;
            if (request.target == clipboard_targets_atom_) {
                std::array<Atom, 4> targets{
                    clipboard_targets_atom_,
                    clipboard_utf8_string_atom_,
                    clipboard_text_atom_,
                    clipboard_string_atom_,
                };
                XChangeProperty(display_, request.requestor, property, XA_ATOM, 32, PropModeReplace,
                                reinterpret_cast<const unsigned char*>(targets.data()),
                                static_cast<int>(targets.size()));
                response.property = property;
            } else if (request.target == clipboard_utf8_string_atom_ ||
                       request.target == clipboard_text_atom_ ||
                       request.target == clipboard_string_atom_) {
                const auto property_type = request.target == clipboard_string_atom_
                                               ? clipboard_string_atom_
                                               : clipboard_utf8_string_atom_;
                if (clipboard_text_.size() > clipboard_direct_transfer_limit_bytes) {
                    begin_incremental_clipboard_transfer(request.requestor, property,
                                                         property_type);
                } else {
                    XChangeProperty(display_, request.requestor, property, property_type, 8,
                                    PropModeReplace,
                                    reinterpret_cast<const unsigned char*>(clipboard_text_.data()),
                                    static_cast<int>(clipboard_text_.size()));
                }
                response.property = property;
            }
        }

        if (request.requestor != 0) {
            XEvent event{};
            event.xselection = response;
            XSendEvent(display_, request.requestor, False, 0, &event);
            XFlush(display_);
        }
    }

    void begin_incremental_clipboard_transfer(::Window requestor, Atom property,
                                              Atom property_type) {
        if (requestor == 0 || property == None) {
            return;
        }

        const auto byte_count = static_cast<long>(clipboard_text_.size());
        XSelectInput(display_, requestor, PropertyChangeMask);
        XChangeProperty(display_, requestor, property, clipboard_incr_atom_, 32, PropModeReplace,
                        reinterpret_cast<const unsigned char*>(&byte_count), 1);
        pending_clipboard_transfers_.push_back(X11OutgoingIncrementalClipboardTransfer{
            requestor, property, property_type, clipboard_text_, 0});
    }

    void handle_property_notify(const XPropertyEvent& event) {
        if (event.state != PropertyDelete) {
            return;
        }

        auto found = std::ranges::find_if(
            pending_clipboard_transfers_,
            [&event](const X11OutgoingIncrementalClipboardTransfer& transfer) {
                return transfer.requestor == event.window && transfer.property == event.atom;
            });
        if (found == pending_clipboard_transfers_.end()) {
            return;
        }

        const auto transfer_complete = send_next_incremental_clipboard_chunk(*found);
        if (transfer_complete) {
            pending_clipboard_transfers_.erase(found);
        }
    }

    [[nodiscard]] bool
    send_next_incremental_clipboard_chunk(X11OutgoingIncrementalClipboardTransfer& transfer) {
        if (transfer.offset >= transfer.bytes.size()) {
            XChangeProperty(display_, transfer.requestor, transfer.property, transfer.property_type,
                            8, PropModeReplace,
                            reinterpret_cast<const unsigned char*>(transfer.bytes.data()), 0);
            XFlush(display_);
            return true;
        }

        const auto remaining = transfer.bytes.size() - transfer.offset;
        const auto chunk_size = std::min(remaining, clipboard_incremental_chunk_bytes);
        XChangeProperty(
            display_, transfer.requestor, transfer.property, transfer.property_type, 8,
            PropModeReplace,
            reinterpret_cast<const unsigned char*>(transfer.bytes.data() + transfer.offset),
            static_cast<int>(chunk_size));
        transfer.offset += chunk_size;
        XFlush(display_);
        return false;
    }

    static constexpr long max_property_read_32bit_units = 256 * 1024;
    static constexpr std::size_t max_clipboard_transfer_bytes = 64U * 1024U * 1024U;
    static constexpr std::size_t clipboard_direct_transfer_limit_bytes = 64U * 1024U;
    static constexpr std::size_t clipboard_incremental_chunk_bytes = 64U * 1024U;
    static constexpr std::int64_t clipboard_incremental_timeout_ms = 2000;

    HeadlessPlatform logical_;
    Display* display_ = nullptr;
    int screen_ = 0;
    Atom wm_delete_window_ = 0;
    Atom clipboard_atom_ = 0;
    Atom clipboard_targets_atom_ = 0;
    Atom clipboard_utf8_string_atom_ = 0;
    Atom clipboard_text_atom_ = 0;
    Atom clipboard_string_atom_ = 0;
    Atom clipboard_incr_atom_ = 0;
    Atom clipboard_transfer_property_atom_ = 0;
    ::Window clipboard_owner_window_ = 0;
    Cursor invisible_cursor_ = None;
    bool owns_clipboard_selection_ = false;
    std::string clipboard_text_;
    std::vector<X11OutgoingIncrementalClipboardTransfer> pending_clipboard_transfers_;
    std::unordered_map<std::uint64_t, ::Window> native_windows_;
    std::unordered_map<::Window, std::uint64_t> window_ids_by_native_;
    std::unordered_set<std::uint64_t> cursor_capture_requests_;
    std::unordered_set<std::uint64_t> unfocused_windows_;
};

[[nodiscard]] core::Result<std::unique_ptr<IPlatform>> create_x11_platform() {
    Display* display = XOpenDisplay(nullptr);
    if (display == nullptr) {
        return core::Result<std::unique_ptr<IPlatform>>::failure("platform.native_unavailable",
                                                                 "X11 display is not available");
    }
    return core::Result<std::unique_ptr<IPlatform>>::success(
        std::make_unique<X11NativePlatform>(display));
}

} // namespace

#endif

core::Status run_headless_app(HeadlessPlatform& platform, const AppRunConfig& config,
                              const HeadlessFrameCallback& frame_callback) {
    if (!frame_callback) {
        return core::Status::failure("platform.missing_frame_callback",
                                     "headless app frame callback is required");
    }
    if (config.max_frames == 0) {
        return core::Status::failure("platform.invalid_max_frames",
                                     "max frame count must be non-zero");
    }

    for (std::uint64_t frame = 0; frame < config.max_frames && !platform.should_quit(); ++frame) {
        platform.begin_frame();
        auto status = frame_callback(platform, frame, platform.clock().now_ms());
        if (!status) {
            return status;
        }
    }

    return core::Status::ok();
}

core::Status run_platform_app(IPlatform& platform, const AppRunConfig& config,
                              const PlatformFrameCallback& frame_callback) {
    if (!frame_callback) {
        return core::Status::failure("platform.missing_frame_callback",
                                     "platform app frame callback is required");
    }
    if (config.max_frames == 0) {
        return core::Status::failure("platform.invalid_max_frames",
                                     "max frame count must be non-zero");
    }

    for (std::uint64_t frame = 0; frame < config.max_frames && !platform.should_quit(); ++frame) {
        platform.begin_frame();
        auto status = frame_callback(platform, frame, platform.clock().now_ms());
        if (!status) {
            return status;
        }
    }

    return core::Status::ok();
}

core::Result<std::unique_ptr<IPlatform>> create_platform(PlatformDesc desc) {
    auto status = validate_platform_desc(desc);
    if (!status) {
        return core::Result<std::unique_ptr<IPlatform>>::failure(status.error().code,
                                                                 status.error().message);
    }

    switch (desc.backend) {
    case PlatformBackend::headless:
        return core::Result<std::unique_ptr<IPlatform>>::success(
            std::make_unique<HeadlessPlatform>());
    case PlatformBackend::native:
#if HEARTSTEAD_HAS_X11
        return create_x11_platform();
#else
        return core::Result<std::unique_ptr<IPlatform>>::failure(
            "platform.native_unavailable", "native platform backend is not compiled in yet");
#endif
    }

    return core::Result<std::unique_ptr<IPlatform>>::failure("platform.unknown_backend",
                                                             "unknown platform backend");
}

core::Status validate_platform_desc(const PlatformDesc& desc) {
    switch (desc.backend) {
    case PlatformBackend::headless:
    case PlatformBackend::native:
        return core::Status::ok();
    }
    return core::Status::failure("platform.unknown_backend", "unknown platform backend");
}

core::Status validate_window_desc(const WindowDesc& desc) {
    if (desc.title.empty()) {
        return core::Status::failure("platform.invalid_window_title",
                                     "window title must not be empty");
    }
    if (desc.width == 0 || desc.height == 0) {
        return core::Status::failure("platform.invalid_window_size",
                                     "window dimensions must be non-zero");
    }
    return core::Status::ok();
}

PlatformBackendInfo platform_backend_info(PlatformBackend backend) noexcept {
    switch (backend) {
    case PlatformBackend::headless:
        return PlatformBackendInfo{
            PlatformBackend::headless,
            platform_backend_name(PlatformBackend::headless),
            true,
            "available",
        };
    case PlatformBackend::native:
#if HEARTSTEAD_HAS_X11
        if (x11_display_available()) {
            return PlatformBackendInfo{
                PlatformBackend::native,
                platform_backend_name(PlatformBackend::native),
                true,
                "available",
            };
        }
        return PlatformBackendInfo{
            PlatformBackend::native,
            platform_backend_name(PlatformBackend::native),
            false,
            "X11 backend is compiled but no display is available",
        };
#else
        return PlatformBackendInfo{
            PlatformBackend::native,
            platform_backend_name(PlatformBackend::native),
            false,
            "native platform backend is not compiled in yet",
        };
#endif
    }
    return PlatformBackendInfo{backend, "unknown", false, "unknown platform backend"};
}

PlatformBackendCapabilities platform_backend_capabilities(PlatformBackend backend) noexcept {
    const auto info = platform_backend_info(backend);
    switch (backend) {
    case PlatformBackend::headless:
        return PlatformBackendCapabilities{
            PlatformBackend::headless,
            info.available,
            true,
            true,
            false,
            true,
            true,
            true,
            true,
            false,
            true,
            "headless",
        };
    case PlatformBackend::native:
#if HEARTSTEAD_HAS_X11
        return PlatformBackendCapabilities{
            PlatformBackend::native,
            info.available,
            false,
            true,
            true,
            true,
            true,
            true,
            true,
            true,
            true,
            "x11",
        };
#else
        return PlatformBackendCapabilities{
            PlatformBackend::native,
            info.available,
            false,
            false,
            false,
            false,
            false,
            false,
            false,
            false,
            false,
            "sdl3_or_equivalent",
        };
#endif
    }
    return PlatformBackendCapabilities{
        backend, false, false, false, false, false, false, false, false, false, false, "unknown",
    };
}

std::string_view platform_backend_name(PlatformBackend backend) noexcept {
    switch (backend) {
    case PlatformBackend::headless:
        return "headless";
    case PlatformBackend::native:
        return "native";
    }
    return "unknown";
}

std::string_view platform_event_kind_name(PlatformEventKind kind) noexcept {
    switch (kind) {
    case PlatformEventKind::quit_requested:
        return "quit_requested";
    case PlatformEventKind::window_created:
        return "window_created";
    case PlatformEventKind::window_closed:
        return "window_closed";
    case PlatformEventKind::window_resized:
        return "window_resized";
    case PlatformEventKind::window_focus_gained:
        return "window_focus_gained";
    case PlatformEventKind::window_focus_lost:
        return "window_focus_lost";
    case PlatformEventKind::key_down:
        return "key_down";
    case PlatformEventKind::key_up:
        return "key_up";
    case PlatformEventKind::text_input:
        return "text_input";
    case PlatformEventKind::mouse_moved:
        return "mouse_moved";
    case PlatformEventKind::mouse_button_down:
        return "mouse_button_down";
    case PlatformEventKind::mouse_button_up:
        return "mouse_button_up";
    case PlatformEventKind::mouse_wheel:
        return "mouse_wheel";
    }
    return "unknown";
}

std::string_view key_code_name(KeyCode key) noexcept {
    switch (key) {
    case KeyCode::unknown:
        return "unknown";
    case KeyCode::escape:
        return "escape";
    case KeyCode::enter:
        return "enter";
    case KeyCode::tab:
        return "tab";
    case KeyCode::space:
        return "space";
    case KeyCode::backspace:
        return "backspace";
    case KeyCode::delete_key:
        return "delete";
    case KeyCode::arrow_left:
        return "left";
    case KeyCode::arrow_right:
        return "right";
    case KeyCode::arrow_up:
        return "up";
    case KeyCode::arrow_down:
        return "down";
    case KeyCode::digit_1:
        return "1";
    case KeyCode::digit_2:
        return "2";
    case KeyCode::digit_3:
        return "3";
    case KeyCode::digit_4:
        return "4";
    case KeyCode::digit_5:
        return "5";
    case KeyCode::digit_6:
        return "6";
    case KeyCode::digit_7:
        return "7";
    case KeyCode::digit_8:
        return "8";
    case KeyCode::digit_9:
        return "9";
    case KeyCode::w:
        return "w";
    case KeyCode::a:
        return "a";
    case KeyCode::s:
        return "s";
    case KeyCode::d:
        return "d";
    case KeyCode::q:
        return "q";
    case KeyCode::e:
        return "e";
    case KeyCode::r:
        return "r";
    case KeyCode::m:
        return "m";
    case KeyCode::left_shift:
        return "left_shift";
    case KeyCode::left_control:
        return "left_control";
    case KeyCode::left_alt:
        return "left_alt";
    case KeyCode::f1:
        return "f1";
    case KeyCode::f3:
        return "f3";
    case KeyCode::f4:
        return "f4";
    case KeyCode::f5:
        return "f5";
    case KeyCode::f7:
        return "f7";
    }
    return "unknown";
}

std::string_view mouse_button_name(MouseButton button) noexcept {
    switch (button) {
    case MouseButton::unknown:
        return "unknown";
    case MouseButton::left:
        return "left";
    case MouseButton::right:
        return "right";
    case MouseButton::middle:
        return "middle";
    case MouseButton::x1:
        return "x1";
    case MouseButton::x2:
        return "x2";
    }
    return "unknown";
}

} // namespace heartstead::platform
