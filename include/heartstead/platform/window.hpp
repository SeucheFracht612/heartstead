#pragma once

#include <cstdint>

struct GLFWwindow;

namespace heartstead::platform {

struct WindowedPlacement {
    std::int32_t x{};
    std::int32_t y{};
    std::int32_t width{1280};
    std::int32_t height{720};
    bool valid{};
};

[[nodiscard]] double process_cpu_seconds() noexcept;
void apply_fullscreen(GLFWwindow* window, bool fullscreen, WindowedPlacement& windowed);

} // namespace heartstead::platform
