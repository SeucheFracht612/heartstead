#pragma once

#include "engine/core/result.hpp"
#include "engine/save/save_database.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace heartstead::save {

struct SaveSlotMetadata {
    std::string slot_id;
    std::string display_name;
    std::uint64_t created_at_ms = 0;
    std::uint64_t last_saved_at_ms = 0;

    [[nodiscard]] core::Status validate() const;
};

struct SaveSlotSummary {
    std::string slot_id;
    std::filesystem::path path;
    SaveSlotMetadata metadata;
    SaveDatabaseStats database_stats;
    std::optional<SaveMetadata> snapshot_metadata;
    std::optional<core::Error> validation_error;
};

struct SaveSlotCatalogSummary {
    std::filesystem::path root;
    std::vector<SaveSlotSummary> slots;
};

class FileSaveSlotCatalog {
  public:
    explicit FileSaveSlotCatalog(std::filesystem::path root);

    [[nodiscard]] const std::filesystem::path& root() const noexcept;

    [[nodiscard]] core::Status create_slot(std::string_view slot_id) const;
    [[nodiscard]] core::Result<FileSaveDatabase> database(std::string_view slot_id) const;
    [[nodiscard]] core::Status write_snapshot(std::string_view slot_id,
                                              const SaveSnapshot& snapshot,
                                              std::uint64_t saved_at_ms) const;
    [[nodiscard]] core::Status write_metadata(const SaveSlotMetadata& metadata) const;
    [[nodiscard]] core::Status rename_slot(std::string_view slot_id,
                                           std::string display_name) const;
    [[nodiscard]] core::Status duplicate_slot(std::string_view source_slot_id,
                                              std::string_view destination_slot_id,
                                              std::string display_name,
                                              std::uint64_t saved_at_ms) const;
    [[nodiscard]] core::Status delete_slot(std::string_view slot_id) const;
    [[nodiscard]] core::Result<SaveSlotMetadata> read_metadata(std::string_view slot_id) const;
    [[nodiscard]] core::Result<std::vector<SaveSlotSummary>> list_slots() const;
    [[nodiscard]] core::Result<SaveSlotCatalogSummary> summary() const;

    [[nodiscard]] static bool is_valid_slot_id(std::string_view slot_id) noexcept;

  private:
    std::filesystem::path root_;
};

} // namespace heartstead::save
