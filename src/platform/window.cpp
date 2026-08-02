#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include "heartstead/platform/window.hpp"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

#include <algorithm>
#include <ctime>

namespace heartstead::platform {
namespace {

[[nodiscard]] GLFWmonitor* monitor_for_window(GLFWwindow* window) noexcept {
    std::int32_t window_x = 0;
    std::int32_t window_y = 0;
    std::int32_t window_width = 0;
    std::int32_t window_height = 0;
    glfwGetWindowPos(window, &window_x, &window_y);
    glfwGetWindowSize(window, &window_width, &window_height);

    std::int32_t monitor_count = 0;
    auto** monitors = glfwGetMonitors(&monitor_count);
    GLFWmonitor* best_monitor = glfwGetPrimaryMonitor();
    std::int64_t best_overlap = -1;
    for (std::int32_t index = 0; index < monitor_count; ++index) {
        auto* monitor = monitors[index];
        std::int32_t monitor_x = 0;
        std::int32_t monitor_y = 0;
        glfwGetMonitorPos(monitor, &monitor_x, &monitor_y);
        const auto* mode = glfwGetVideoMode(monitor);
        if (!mode) continue;
        const auto overlap_width = std::max(0,
            std::min(window_x + window_width, monitor_x + mode->width) - std::max(window_x, monitor_x));
        const auto overlap_height = std::max(0,
            std::min(window_y + window_height, monitor_y + mode->height) - std::max(window_y, monitor_y));
        const auto overlap = static_cast<std::int64_t>(overlap_width) * overlap_height;
        if (overlap > best_overlap) {
            best_overlap = overlap;
            best_monitor = monitor;
        }
    }
    return best_monitor;
}

} // namespace

double process_cpu_seconds() noexcept {
#if defined(_WIN32)
    FILETIME creation_time{};
    FILETIME exit_time{};
    FILETIME kernel_time{};
    FILETIME user_time{};
    if (GetProcessTimes(GetCurrentProcess(), &creation_time, &exit_time, &kernel_time, &user_time) == 0)
        return 0.0;
    ULARGE_INTEGER kernel{};
    kernel.LowPart = kernel_time.dwLowDateTime;
    kernel.HighPart = kernel_time.dwHighDateTime;
    ULARGE_INTEGER user{};
    user.LowPart = user_time.dwLowDateTime;
    user.HighPart = user_time.dwHighDateTime;
    return static_cast<double>(kernel.QuadPart + user.QuadPart) * 1.0e-7;
#else
    return static_cast<double>(std::clock()) / static_cast<double>(CLOCKS_PER_SEC);
#endif
}

void apply_fullscreen(GLFWwindow* window, bool fullscreen, WindowedPlacement& windowed) {
    const auto currently_fullscreen = glfwGetWindowMonitor(window) != nullptr;
    if (fullscreen == currently_fullscreen) return;
    if (fullscreen) {
        glfwGetWindowPos(window, &windowed.x, &windowed.y);
        glfwGetWindowSize(window, &windowed.width, &windowed.height);
        windowed.valid = true;
        auto* monitor = monitor_for_window(window);
        const auto* mode = monitor ? glfwGetVideoMode(monitor) : nullptr;
        if (monitor && mode) {
            glfwSetWindowMonitor(window, monitor, 0, 0,
                mode->width, mode->height, mode->refreshRate);
        }
        return;
    }
    glfwSetWindowMonitor(window, nullptr,
        windowed.valid ? windowed.x : 100,
        windowed.valid ? windowed.y : 100,
        windowed.valid ? windowed.width : 1280,
        windowed.valid ? windowed.height : 720,
        GLFW_DONT_CARE);
}

} // namespace heartstead::platform
