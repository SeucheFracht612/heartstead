#pragma once

#include "heartstead/game/player.hpp"
#include "heartstead/world/world_generation.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace heartstead::app {

struct WorldSaveSummary {
    std::string id;
    std::string name;
    std::string created;
    std::string last_played;
};

struct WorldSaveData {
    WorldSaveSummary summary;
    world::WorldEdits edits;
    Float3 player_position{0.5F, 40.0F, 0.5F};
    float player_yaw{-1.57079633F};
    float camera_yaw{-1.57079633F};
    float camera_pitch{-0.22F};
};

class WorldSaveStore {
public:
    explicit WorldSaveStore(std::filesystem::path root = "saves");

    [[nodiscard]] std::vector<WorldSaveSummary> list() const;
    [[nodiscard]] WorldSaveData create(std::string name, std::string created) const;
    [[nodiscard]] std::optional<WorldSaveData> load(const std::string& id) const;
    [[nodiscard]] bool save(WorldSaveData& data) const;

private:
    [[nodiscard]] std::filesystem::path path_for(const std::string& id) const;

    std::filesystem::path root_;
};

} // namespace heartstead::app
