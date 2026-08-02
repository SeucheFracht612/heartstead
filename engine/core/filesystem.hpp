#pragma once

#include "engine/core/result.hpp"

#include <cstddef>
#include <filesystem>
#include <system_error>
#include <vector>

namespace heartstead::core {

// Replaces destination with a fully written staged file. The staged file must be on the same
// filesystem as the destination. Failure leaves an existing destination in place.
[[nodiscard]] std::error_code replace_file(const std::filesystem::path& staged,
                                           const std::filesystem::path& destination) noexcept;

// Requests stable-storage completion for an existing regular file. This operation is blocking
// and belongs on an I/O worker when it is used by runtime persistence.
[[nodiscard]] std::error_code
flush_file_to_disk(const std::filesystem::path& path) noexcept;

// Requests persistence of directory-entry changes where the platform exposes that operation.
// Windows durable replacement uses MOVEFILE_WRITE_THROUGH and this function is a no-op there.
[[nodiscard]] std::error_code
flush_directory_to_disk(const std::filesystem::path& path) noexcept;

// Flushes the completed staged file, atomically installs it, and persists the destination
// directory entry. The staged file must be on the same filesystem as the destination.
[[nodiscard]] std::error_code
replace_file_durable(const std::filesystem::path& staged,
                     const std::filesystem::path& destination) noexcept;

struct RecursiveFileListOptions {
    std::size_t maximum_entries = 65'536;
    std::size_t maximum_depth = 32;
    bool reject_symbolic_links = true;
};

struct DirectoryListOptions {
    std::size_t maximum_entries = 4'096;
    bool reject_symbolic_links = true;
};

struct FileListOptions {
    std::size_t maximum_entries = 4'096;
    bool reject_symbolic_links = true;
};

// Resolves an existing candidate and returns its canonical path relative to an existing root.
// Both the lexical and resolved path must remain below the root. The final candidate may also be
// required not to be a symbolic link.
[[nodiscard]] Result<std::filesystem::path>
relative_path_below(const std::filesystem::path& root, const std::filesystem::path& candidate,
                    bool reject_symbolic_link = true);

// Enumerates a bounded tree without following directory links. When symbolic links are rejected,
// encountering one fails the whole operation instead of silently importing aliased content.
[[nodiscard]] Result<std::vector<std::filesystem::path>>
list_regular_files_recursive(const std::filesystem::path& root,
                             RecursiveFileListOptions options = {});

// Lists immediate child directories with deterministic ordering and a bound on every inspected
// entry, including non-directory entries.
[[nodiscard]] Result<std::vector<std::filesystem::path>>
list_directories(const std::filesystem::path& root, DirectoryListOptions options = {});

// Lists immediate regular-file children with deterministic ordering and a bound on every
// inspected entry, including directories and other non-regular entries.
[[nodiscard]] Result<std::vector<std::filesystem::path>>
list_regular_files(const std::filesystem::path& root, FileListOptions options = {});

} // namespace heartstead::core
