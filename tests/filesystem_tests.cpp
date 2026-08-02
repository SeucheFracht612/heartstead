#include "engine/core/filesystem.hpp"

#include <cassert>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>

namespace {

[[nodiscard]] std::filesystem::path make_temp_root() {
    const auto parent = std::filesystem::temp_directory_path();
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    for (std::uint32_t attempt = 0; attempt < 100; ++attempt) {
        const auto root = parent / ("heartstead_replace_file_" + std::to_string(nonce) + "_" +
                                    std::to_string(attempt));
        std::error_code error;
        if (std::filesystem::create_directory(root, error)) {
            return root;
        }
    }
    assert(false && "could not create filesystem test directory");
    return {};
}

void write_text(const std::filesystem::path& path, std::string_view text) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    assert(output);
    output << text;
    output.close();
    assert(output);
}

[[nodiscard]] std::string read_text(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    assert(input);
    std::ostringstream text;
    text << input.rdbuf();
    assert(!input.bad());
    return text.str();
}

void test_replace_file_installs_staged_content_and_preserves_on_failure() {
    const auto root = make_temp_root();
    const auto destination = root / "state.txt";
    const auto staged = root / "state.txt.tmp";

    write_text(destination, "old");
    write_text(staged, "new");
    assert(!heartstead::core::replace_file(staged, destination));
    assert(!std::filesystem::exists(staged));
    assert(read_text(destination) == "new");

    const auto missing = root / "missing.tmp";
    assert(heartstead::core::replace_file(missing, destination));
    assert(read_text(destination) == "new");

    const auto initially_missing = root / "created.txt";
    write_text(staged, "created");
    assert(!heartstead::core::replace_file(staged, initially_missing));
    assert(read_text(initially_missing) == "created");

    std::error_code cleanup_error;
    std::filesystem::remove_all(root, cleanup_error);
    assert(!cleanup_error);
}

void test_durable_replace_flushes_file_and_directory() {
    const auto root = make_temp_root();
    const auto destination = root / "durable.txt";
    const auto staged = root / "durable.txt.tmp";

    write_text(staged, "durable");
    assert(!heartstead::core::flush_file_to_disk(staged));
    assert(!heartstead::core::flush_directory_to_disk(root));
    assert(!heartstead::core::replace_file_durable(staged, destination));
    assert(!std::filesystem::exists(staged));
    assert(read_text(destination) == "durable");

    const auto missing = root / "missing.tmp";
    assert(heartstead::core::replace_file_durable(missing, destination));
    assert(read_text(destination) == "durable");

    std::error_code cleanup_error;
    std::filesystem::remove_all(root, cleanup_error);
    assert(!cleanup_error);
}

void test_bounded_recursive_listing_and_root_confinement() {
    const auto root = make_temp_root();
    std::filesystem::create_directories(root / "nested/deeper");
    write_text(root / "top.txt", "top");
    write_text(root / "nested/deeper/value.txt", "value");

    const auto listed = heartstead::core::list_regular_files_recursive(root);
    assert(listed);
    assert(listed.value().size() == 2);
    assert(listed.value().front().lexically_relative(root) ==
           std::filesystem::path("nested/deeper/value.txt"));
    assert(listed.value().back().lexically_relative(root) == std::filesystem::path("top.txt"));

    const auto relative =
        heartstead::core::relative_path_below(root, root / "nested/deeper/value.txt");
    assert(relative);
    assert(relative.value() == std::filesystem::path("nested/deeper/value.txt"));
    const auto outside = heartstead::core::relative_path_below(root, root.parent_path());
    assert(!outside);
    assert(outside.error().code == "core.path_outside_root");

    const auto entry_limited = heartstead::core::list_regular_files_recursive(
        root, {.maximum_entries = 1, .maximum_depth = 32});
    assert(!entry_limited);
    assert(entry_limited.error().code == "core.directory_too_large");
    const auto depth_limited = heartstead::core::list_regular_files_recursive(
        root, {.maximum_entries = 32, .maximum_depth = 1});
    assert(!depth_limited);
    assert(depth_limited.error().code == "core.directory_too_deep");

    const auto directories = heartstead::core::list_directories(root);
    assert(directories);
    assert(directories.value().size() == 1);
    assert(directories.value().front().filename() == "nested");
    const auto directory_limited = heartstead::core::list_directories(root, {.maximum_entries = 1});
    assert(!directory_limited);
    assert(directory_limited.error().code == "core.directory_too_large");

    const auto files = heartstead::core::list_regular_files(root);
    assert(files);
    assert(files.value().size() == 1);
    assert(files.value().front().filename() == "top.txt");
    const auto file_limited = heartstead::core::list_regular_files(root, {.maximum_entries = 1});
    assert(!file_limited);
    assert(file_limited.error().code == "core.directory_too_large");

    std::error_code link_error;
    std::filesystem::create_symlink(root / "top.txt", root / "linked.txt", link_error);
    if (!link_error) {
        const auto linked = heartstead::core::list_regular_files_recursive(root);
        assert(!linked);
        assert(linked.error().code == "core.directory_symlink_forbidden");
        const auto linked_directories = heartstead::core::list_directories(root);
        assert(!linked_directories);
        assert(linked_directories.error().code == "core.directory_symlink_forbidden");
        const auto linked_files = heartstead::core::list_regular_files(root);
        assert(!linked_files);
        assert(linked_files.error().code == "core.directory_symlink_forbidden");
        const auto linked_relative =
            heartstead::core::relative_path_below(root, root / "linked.txt");
        assert(!linked_relative);
        assert(linked_relative.error().code == "core.path_symlink_forbidden");
    }

    std::error_code cleanup_error;
    std::filesystem::remove_all(root, cleanup_error);
    assert(!cleanup_error);
}

} // namespace

int main() {
    test_replace_file_installs_staged_content_and_preserves_on_failure();
    test_durable_replace_flushes_file_and_directory();
    test_bounded_recursive_listing_and_root_confinement();
    return 0;
}
