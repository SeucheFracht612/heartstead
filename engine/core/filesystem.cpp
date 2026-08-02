#include "engine/core/filesystem.hpp"

#include <algorithm>
#include <cerrno>
#include <string>
#include <string_view>
#include <utility>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace heartstead::core {

namespace {

[[nodiscard]] bool same_or_child(const std::filesystem::path& child,
                                 const std::filesystem::path& parent) noexcept {
    auto child_part = child.begin();
    for (auto parent_part = parent.begin(); parent_part != parent.end();
         ++parent_part, ++child_part) {
        if (child_part == child.end() || *child_part != *parent_part) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] Result<std::filesystem::path> canonical_existing(const std::filesystem::path& path,
                                                               std::string_view label) {
    std::error_code error;
    const auto canonical = std::filesystem::canonical(path, error);
    if (error) {
        return Result<std::filesystem::path>::failure(
            "core.path_resolution_failed", "failed to resolve existing " + std::string(label) +
                                               ": " + path.string() + ": " + error.message());
    }
    return Result<std::filesystem::path>::success(canonical);
}

} // namespace

std::error_code replace_file(const std::filesystem::path& staged,
                             const std::filesystem::path& destination) noexcept {
#ifdef _WIN32
    if (::MoveFileExW(staged.c_str(), destination.c_str(),
                      MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == 0) {
        return {static_cast<int>(::GetLastError()), std::system_category()};
    }
    return {};
#else
    std::error_code error;
    std::filesystem::rename(staged, destination, error);
    return error;
#endif
}

std::error_code flush_file_to_disk(const std::filesystem::path& path) noexcept {
#ifdef _WIN32
    const auto handle = ::CreateFileW(path.c_str(), GENERIC_READ,
                                      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                      nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        return {static_cast<int>(::GetLastError()), std::system_category()};
    }
    if (::FlushFileBuffers(handle) == 0) {
        const auto error = std::error_code(static_cast<int>(::GetLastError()),
                                           std::system_category());
        static_cast<void>(::CloseHandle(handle));
        return error;
    }
    if (::CloseHandle(handle) == 0) {
        return {static_cast<int>(::GetLastError()), std::system_category()};
    }
    return {};
#else
    const auto descriptor = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (descriptor < 0) {
        return {errno, std::generic_category()};
    }
    if (::fsync(descriptor) != 0) {
        const auto error = std::error_code(errno, std::generic_category());
        static_cast<void>(::close(descriptor));
        return error;
    }
    if (::close(descriptor) != 0) {
        return {errno, std::generic_category()};
    }
    return {};
#endif
}

std::error_code flush_directory_to_disk(const std::filesystem::path& path) noexcept {
#ifdef _WIN32
    // MoveFileExW with MOVEFILE_WRITE_THROUGH is the supported durable-replacement primitive used
    // by replace_file(). Windows does not provide a portable equivalent of POSIX directory fsync.
    static_cast<void>(path);
    return {};
#else
    const auto directory = path.empty() ? std::filesystem::path(".") : path;
    const auto descriptor = ::open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (descriptor < 0) {
        return {errno, std::generic_category()};
    }
    if (::fsync(descriptor) != 0) {
        const auto error = std::error_code(errno, std::generic_category());
        static_cast<void>(::close(descriptor));
        return error;
    }
    if (::close(descriptor) != 0) {
        return {errno, std::generic_category()};
    }
    return {};
#endif
}

std::error_code replace_file_durable(const std::filesystem::path& staged,
                                     const std::filesystem::path& destination) noexcept {
    if (auto error = flush_file_to_disk(staged)) {
        return error;
    }
    if (auto error = replace_file(staged, destination)) {
        return error;
    }
    return flush_directory_to_disk(destination.parent_path());
}

Result<std::filesystem::path> relative_path_below(const std::filesystem::path& root,
                                                  const std::filesystem::path& candidate,
                                                  bool reject_symbolic_link) {
    std::error_code error;
    const auto lexical_root = std::filesystem::absolute(root, error).lexically_normal();
    if (error) {
        return Result<std::filesystem::path>::failure(
            "core.path_resolution_failed",
            "failed to resolve filesystem root: " + root.string() + ": " + error.message());
    }
    const auto lexical_candidate = std::filesystem::absolute(candidate, error).lexically_normal();
    if (error) {
        return Result<std::filesystem::path>::failure(
            "core.path_resolution_failed", "failed to resolve filesystem candidate: " +
                                               candidate.string() + ": " + error.message());
    }
    if (!same_or_child(lexical_candidate, lexical_root) || lexical_candidate == lexical_root) {
        return Result<std::filesystem::path>::failure(
            "core.path_outside_root",
            "filesystem candidate is outside its lexical root: " + candidate.string());
    }

    auto canonical_root = canonical_existing(root, "filesystem root");
    if (!canonical_root) {
        return canonical_root;
    }
    if (!std::filesystem::is_directory(canonical_root.value(), error) || error) {
        return Result<std::filesystem::path>::failure(
            "core.path_root_not_directory", "filesystem root is not a directory: " + root.string());
    }

    const auto candidate_status = std::filesystem::symlink_status(candidate, error);
    if (error) {
        return Result<std::filesystem::path>::failure(
            "core.path_status_failed", "failed to inspect filesystem candidate: " +
                                           candidate.string() + ": " + error.message());
    }
    if (reject_symbolic_link && std::filesystem::is_symlink(candidate_status)) {
        return Result<std::filesystem::path>::failure(
            "core.path_symlink_forbidden",
            "filesystem candidate must not be a symbolic link: " + candidate.string());
    }

    auto canonical_candidate = canonical_existing(candidate, "filesystem candidate");
    if (!canonical_candidate) {
        return canonical_candidate;
    }
    if (!same_or_child(canonical_candidate.value(), canonical_root.value()) ||
        canonical_candidate.value() == canonical_root.value()) {
        return Result<std::filesystem::path>::failure(
            "core.path_outside_root",
            "filesystem candidate resolves outside its root: " + candidate.string());
    }

    const auto relative = canonical_candidate.value().lexically_relative(canonical_root.value());
    if (relative.empty() || relative == ".") {
        return Result<std::filesystem::path>::failure(
            "core.path_relative_failed",
            "failed to derive a relative filesystem candidate path: " + candidate.string());
    }
    return Result<std::filesystem::path>::success(relative);
}

Result<std::vector<std::filesystem::path>>
list_regular_files_recursive(const std::filesystem::path& root, RecursiveFileListOptions options) {
    if (options.maximum_entries == 0 || options.maximum_depth == 0) {
        return Result<std::vector<std::filesystem::path>>::failure(
            "core.directory_invalid_limit",
            "recursive directory entry and depth limits must be non-zero");
    }

    std::error_code error;
    const auto root_status = std::filesystem::symlink_status(root, error);
    if (error) {
        if (error == std::errc::no_such_file_or_directory) {
            return Result<std::vector<std::filesystem::path>>::failure(
                "core.directory_not_directory",
                "recursive directory root is not a directory: " + root.string());
        }
        return Result<std::vector<std::filesystem::path>>::failure(
            "core.directory_status_failed", "failed to inspect recursive directory root: " +
                                                root.string() + ": " + error.message());
    }
    if (options.reject_symbolic_links && std::filesystem::is_symlink(root_status)) {
        return Result<std::vector<std::filesystem::path>>::failure(
            "core.directory_symlink_forbidden",
            "recursive directory root must not be a symbolic link: " + root.string());
    }
    if (!std::filesystem::is_directory(root_status)) {
        return Result<std::vector<std::filesystem::path>>::failure(
            "core.directory_not_directory",
            "recursive directory root is not a directory: " + root.string());
    }

    auto canonical_root = canonical_existing(root, "recursive directory root");
    if (!canonical_root) {
        return Result<std::vector<std::filesystem::path>>::failure(canonical_root.error().code,
                                                                   canonical_root.error().message);
    }

    std::filesystem::recursive_directory_iterator iterator(canonical_root.value(), error);
    if (error) {
        return Result<std::vector<std::filesystem::path>>::failure(
            "core.directory_list_failed",
            "failed to open recursive directory: " + root.string() + ": " + error.message());
    }

    std::vector<std::filesystem::path> files;
    std::size_t entry_count = 0;
    const std::filesystem::recursive_directory_iterator end;
    while (iterator != end) {
        ++entry_count;
        if (entry_count > options.maximum_entries) {
            return Result<std::vector<std::filesystem::path>>::failure(
                "core.directory_too_large",
                "recursive directory exceeds its entry limit: " + root.string());
        }
        if (static_cast<std::size_t>(iterator.depth()) + 1U > options.maximum_depth) {
            return Result<std::vector<std::filesystem::path>>::failure(
                "core.directory_too_deep",
                "recursive directory exceeds its depth limit: " + iterator->path().string());
        }

        const auto status = iterator->symlink_status(error);
        if (error) {
            return Result<std::vector<std::filesystem::path>>::failure(
                "core.directory_status_failed",
                "failed to inspect recursive directory entry: " + iterator->path().string() + ": " +
                    error.message());
        }
        if (std::filesystem::is_symlink(status)) {
            if (options.reject_symbolic_links) {
                return Result<std::vector<std::filesystem::path>>::failure(
                    "core.directory_symlink_forbidden",
                    "recursive directory contains a symbolic link: " + iterator->path().string());
            }
            iterator.disable_recursion_pending();
        } else if (std::filesystem::is_regular_file(status)) {
            files.push_back(iterator->path());
        }

        iterator.increment(error);
        if (error) {
            return Result<std::vector<std::filesystem::path>>::failure(
                "core.directory_list_failed", "failed while traversing recursive directory: " +
                                                  root.string() + ": " + error.message());
        }
    }

    std::ranges::sort(files);
    return Result<std::vector<std::filesystem::path>>::success(std::move(files));
}

Result<std::vector<std::filesystem::path>> list_directories(const std::filesystem::path& root,
                                                            DirectoryListOptions options) {
    if (options.maximum_entries == 0) {
        return Result<std::vector<std::filesystem::path>>::failure(
            "core.directory_invalid_limit", "directory entry limit must be non-zero");
    }

    std::error_code error;
    const auto root_status = std::filesystem::symlink_status(root, error);
    if (error) {
        if (error == std::errc::no_such_file_or_directory) {
            return Result<std::vector<std::filesystem::path>>::failure(
                "core.directory_not_directory",
                "directory root is not a directory: " + root.string());
        }
        return Result<std::vector<std::filesystem::path>>::failure(
            "core.directory_status_failed",
            "failed to inspect directory root: " + root.string() + ": " + error.message());
    }
    if (options.reject_symbolic_links && std::filesystem::is_symlink(root_status)) {
        return Result<std::vector<std::filesystem::path>>::failure(
            "core.directory_symlink_forbidden",
            "directory root must not be a symbolic link: " + root.string());
    }
    if (!std::filesystem::is_directory(root_status)) {
        return Result<std::vector<std::filesystem::path>>::failure(
            "core.directory_not_directory", "directory root is not a directory: " + root.string());
    }

    auto canonical_root = canonical_existing(root, "directory root");
    if (!canonical_root) {
        return Result<std::vector<std::filesystem::path>>::failure(canonical_root.error().code,
                                                                   canonical_root.error().message);
    }

    std::filesystem::directory_iterator iterator(canonical_root.value(), error);
    if (error) {
        return Result<std::vector<std::filesystem::path>>::failure(
            "core.directory_list_failed",
            "failed to open directory: " + root.string() + ": " + error.message());
    }

    std::vector<std::filesystem::path> directories;
    std::size_t entry_count = 0;
    const std::filesystem::directory_iterator end;
    while (iterator != end) {
        ++entry_count;
        if (entry_count > options.maximum_entries) {
            return Result<std::vector<std::filesystem::path>>::failure(
                "core.directory_too_large", "directory exceeds its entry limit: " + root.string());
        }
        const auto status = iterator->symlink_status(error);
        if (error) {
            return Result<std::vector<std::filesystem::path>>::failure(
                "core.directory_status_failed",
                "failed to inspect directory entry: " + iterator->path().string() + ": " +
                    error.message());
        }
        if (std::filesystem::is_symlink(status) && options.reject_symbolic_links) {
            return Result<std::vector<std::filesystem::path>>::failure(
                "core.directory_symlink_forbidden",
                "directory contains a symbolic link: " + iterator->path().string());
        }
        if (std::filesystem::is_directory(status)) {
            directories.push_back(iterator->path());
        }
        iterator.increment(error);
        if (error) {
            return Result<std::vector<std::filesystem::path>>::failure(
                "core.directory_list_failed",
                "failed while listing directory: " + root.string() + ": " + error.message());
        }
    }

    std::ranges::sort(directories);
    return Result<std::vector<std::filesystem::path>>::success(std::move(directories));
}

Result<std::vector<std::filesystem::path>> list_regular_files(const std::filesystem::path& root,
                                                              FileListOptions options) {
    if (options.maximum_entries == 0) {
        return Result<std::vector<std::filesystem::path>>::failure(
            "core.directory_invalid_limit", "directory entry limit must be non-zero");
    }

    std::error_code error;
    const auto root_status = std::filesystem::symlink_status(root, error);
    if (error) {
        if (error == std::errc::no_such_file_or_directory) {
            return Result<std::vector<std::filesystem::path>>::failure(
                "core.directory_not_directory",
                "directory root is not a directory: " + root.string());
        }
        return Result<std::vector<std::filesystem::path>>::failure(
            "core.directory_status_failed",
            "failed to inspect directory root: " + root.string() + ": " + error.message());
    }
    if (options.reject_symbolic_links && std::filesystem::is_symlink(root_status)) {
        return Result<std::vector<std::filesystem::path>>::failure(
            "core.directory_symlink_forbidden",
            "directory root must not be a symbolic link: " + root.string());
    }
    if (!std::filesystem::is_directory(root_status)) {
        return Result<std::vector<std::filesystem::path>>::failure(
            "core.directory_not_directory", "directory root is not a directory: " + root.string());
    }

    auto canonical_root = canonical_existing(root, "directory root");
    if (!canonical_root) {
        return Result<std::vector<std::filesystem::path>>::failure(canonical_root.error().code,
                                                                   canonical_root.error().message);
    }

    std::filesystem::directory_iterator iterator(canonical_root.value(), error);
    if (error) {
        return Result<std::vector<std::filesystem::path>>::failure(
            "core.directory_list_failed",
            "failed to open directory: " + root.string() + ": " + error.message());
    }

    std::vector<std::filesystem::path> files;
    std::size_t entry_count = 0;
    const std::filesystem::directory_iterator end;
    while (iterator != end) {
        ++entry_count;
        if (entry_count > options.maximum_entries) {
            return Result<std::vector<std::filesystem::path>>::failure(
                "core.directory_too_large", "directory exceeds its entry limit: " + root.string());
        }
        const auto status = iterator->symlink_status(error);
        if (error) {
            return Result<std::vector<std::filesystem::path>>::failure(
                "core.directory_status_failed",
                "failed to inspect directory entry: " + iterator->path().string() + ": " +
                    error.message());
        }
        if (std::filesystem::is_symlink(status) && options.reject_symbolic_links) {
            return Result<std::vector<std::filesystem::path>>::failure(
                "core.directory_symlink_forbidden",
                "directory contains a symbolic link: " + iterator->path().string());
        }
        if (std::filesystem::is_regular_file(status)) {
            files.push_back(iterator->path());
        }
        iterator.increment(error);
        if (error) {
            return Result<std::vector<std::filesystem::path>>::failure(
                "core.directory_list_failed",
                "failed while listing directory: " + root.string() + ": " + error.message());
        }
    }

    std::ranges::sort(files);
    return Result<std::vector<std::filesystem::path>>::success(std::move(files));
}

} // namespace heartstead::core
