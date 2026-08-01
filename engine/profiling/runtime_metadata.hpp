#pragma once

#include <cstdint>
#include <string>

namespace heartstead::profiling {

struct RuntimeMetadata {
    std::string engine_version;
    std::string git_commit;
    std::string build_configuration;
    std::string compiler;
    std::string platform;
    std::string architecture;
    std::string operating_system;
    std::string cpu_model;
    std::uint32_t logical_cpu_count = 0;
    bool git_dirty = false;
    bool tracy_enabled = false;
};

[[nodiscard]] RuntimeMetadata query_runtime_metadata();

} // namespace heartstead::profiling
