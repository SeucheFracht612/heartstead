#include "engine/profiling/runtime_metadata.hpp"

#include <algorithm>
#include <fstream>
#include <string_view>
#include <thread>

#if defined(__linux__) || defined(__APPLE__) || defined(__FreeBSD__)
#include <sys/utsname.h>
#endif

#ifndef HEARTSTEAD_BUILD_VERSION
#define HEARTSTEAD_BUILD_VERSION "unknown"
#endif

#ifndef HEARTSTEAD_BUILD_CONFIGURATION
#define HEARTSTEAD_BUILD_CONFIGURATION "unknown"
#endif

#ifndef HEARTSTEAD_BUILD_GIT_COMMIT
#define HEARTSTEAD_BUILD_GIT_COMMIT "unknown"
#endif

#ifndef HEARTSTEAD_BUILD_GIT_DIRTY
#define HEARTSTEAD_BUILD_GIT_DIRTY 0
#endif

#ifndef HEARTSTEAD_HAS_TRACY
#define HEARTSTEAD_HAS_TRACY 0
#endif

namespace heartstead::profiling {

namespace {

[[nodiscard]] std::string compiler_name() {
#if defined(__clang__)
    return "Clang " + std::to_string(__clang_major__) + "." + std::to_string(__clang_minor__) +
           "." + std::to_string(__clang_patchlevel__);
#elif defined(__GNUC__)
    return "GCC " + std::to_string(__GNUC__) + "." + std::to_string(__GNUC_MINOR__) + "." +
           std::to_string(__GNUC_PATCHLEVEL__);
#elif defined(_MSC_VER)
    return "MSVC " + std::to_string(_MSC_VER);
#else
    return "unknown";
#endif
}

[[nodiscard]] constexpr std::string_view platform_name() noexcept {
#if defined(_WIN32)
    return "windows";
#elif defined(__APPLE__)
    return "macos";
#elif defined(__linux__)
    return "linux";
#elif defined(__FreeBSD__)
    return "freebsd";
#else
    return "unknown";
#endif
}

[[nodiscard]] constexpr std::string_view architecture_name() noexcept {
#if defined(__x86_64__) || defined(_M_X64)
    return "x86_64";
#elif defined(__aarch64__) || defined(_M_ARM64)
    return "arm64";
#elif defined(__i386__) || defined(_M_IX86)
    return "x86";
#elif defined(__arm__) || defined(_M_ARM)
    return "arm";
#else
    return "unknown";
#endif
}

[[nodiscard]] std::string operating_system_name() {
#if defined(__linux__) || defined(__APPLE__) || defined(__FreeBSD__)
    utsname information{};
    if (uname(&information) == 0) {
        return std::string(information.sysname) + " " + information.release;
    }
#endif
    return std::string(platform_name());
}

[[nodiscard]] std::string linux_cpu_model() {
#if defined(__linux__)
    std::ifstream input("/proc/cpuinfo");
    std::string line;
    while (std::getline(input, line)) {
        constexpr std::string_view key = "model name";
        if (!line.starts_with(key)) {
            continue;
        }
        const auto separator = line.find(':');
        if (separator == std::string::npos) {
            continue;
        }
        auto value = line.substr(separator + 1);
        const auto first = value.find_first_not_of(" \t");
        if (first != std::string::npos) {
            value.erase(0, first);
        }
        return value;
    }
#endif
    return "unknown";
}

} // namespace

RuntimeMetadata query_runtime_metadata() {
    RuntimeMetadata result;
    result.engine_version = HEARTSTEAD_BUILD_VERSION;
    result.git_commit = HEARTSTEAD_BUILD_GIT_COMMIT;
    result.build_configuration = HEARTSTEAD_BUILD_CONFIGURATION;
    result.compiler = compiler_name();
    result.platform = platform_name();
    result.architecture = architecture_name();
    result.operating_system = operating_system_name();
    result.cpu_model = linux_cpu_model();
    result.logical_cpu_count = std::max(1U, std::thread::hardware_concurrency());
    result.git_dirty = HEARTSTEAD_BUILD_GIT_DIRTY != 0;
    result.tracy_enabled = HEARTSTEAD_HAS_TRACY != 0;
    return result;
}

} // namespace heartstead::profiling
