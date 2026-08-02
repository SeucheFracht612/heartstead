#include "heartstead/app/world_save_store.hpp"

#include "heartstead/voxel/chunk.hpp"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <limits>
#include <string_view>
#include <system_error>
#include <utility>

namespace heartstead::app {
namespace {

[[nodiscard]] std::string local_timestamp() {
    const auto now = std::time(nullptr);
    std::tm local_time{};
#if defined(_WIN32)
    localtime_s(&local_time, &now);
#else
    localtime_r(&now, &local_time);
#endif
    char text[24]{};
    std::strftime(text, sizeof(text), "%Y-%m-%d %H:%M", &local_time);
    return text;
}

[[nodiscard]] std::string save_identifier(std::string_view name) {
    std::string result;
    result.reserve(name.size() + 24U);
    for (const auto character : name) {
        const auto value = static_cast<unsigned char>(character);
        if (std::isalnum(value) != 0) {
            result.push_back(static_cast<char>(std::tolower(value)));
        } else if (!result.empty() && result.back() != '-') {
            result.push_back('-');
        }
    }
    while (!result.empty() && result.back() == '-') result.pop_back();
    if (result.empty()) result = "world";
    const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    result += '-' + std::to_string(milliseconds);
    return result;
}

} // namespace

WorldSaveStore::WorldSaveStore(std::filesystem::path root) : root_(std::move(root)) {}

std::filesystem::path WorldSaveStore::path_for(const std::string& id) const {
    return root_ / (id + ".hws");
}

WorldSaveData WorldSaveStore::create(std::string name, std::string created) const {
    WorldSaveData data;
    data.summary.id = save_identifier(name);
    const auto base_id = data.summary.id;
    std::uint32_t suffix = 1;
    std::error_code error;
    while (std::filesystem::exists(path_for(data.summary.id), error) && !error)
        data.summary.id = base_id + '-' + std::to_string(suffix++);
    data.summary.name = std::move(name);
    data.summary.created = std::move(created);
    data.summary.last_played = local_timestamp();
    return data;
}

bool WorldSaveStore::save(WorldSaveData& data) const {
    std::error_code error;
    std::filesystem::create_directories(root_, error);
    if (error) return false;
    data.summary.last_played = local_timestamp();
    const auto destination = path_for(data.summary.id);
    auto temporary = destination;
    temporary += ".tmp";
    std::ofstream output(temporary, std::ios::trunc);
    if (!output) return false;

    std::size_t edit_count = 0;
    for (const auto& [coordinate, edits] : data.edits.chunks) {
        (void)coordinate;
        edit_count += edits.size();
    }
    output << "HEARTSTEAD_WORLD 1\n"
           << std::quoted(data.summary.id) << '\n'
           << std::quoted(data.summary.name) << '\n'
           << std::quoted(data.summary.created) << '\n'
           << std::quoted(data.summary.last_played) << '\n'
           << std::setprecision(9)
           << data.player_position.x << ' ' << data.player_position.y << ' '
           << data.player_position.z << ' ' << data.player_yaw << ' '
           << data.camera_yaw << ' ' << data.camera_pitch << '\n'
           << edit_count << '\n';
    for (const auto& [coordinate, edits] : data.edits.chunks) {
        for (const auto& [index, block] : edits) {
            output << coordinate.x << ' ' << coordinate.y << ' ' << coordinate.z << ' '
                   << index << ' ' << static_cast<std::uint32_t>(block) << '\n';
        }
    }
    output.flush();
    if (!output) return false;
    output.close();

    auto backup = destination;
    backup += ".bak";
    std::filesystem::remove(backup, error);
    error.clear();
    const auto had_previous = std::filesystem::exists(destination, error) && !error;
    if (had_previous) {
        std::filesystem::rename(destination, backup, error);
        if (error) {
            std::filesystem::remove(temporary, error);
            return false;
        }
    }
    std::filesystem::rename(temporary, destination, error);
    if (!error) {
        std::filesystem::remove(backup, error);
        return true;
    }
    std::filesystem::remove(temporary, error);
    if (had_previous) {
        error.clear();
        std::filesystem::rename(backup, destination, error);
    }
    return false;
}

std::optional<WorldSaveData> WorldSaveStore::load(const std::string& id) const {
    if (id.empty() || id.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789-") != std::string::npos)
        return std::nullopt;
    std::ifstream input(path_for(id));
    if (!input) return std::nullopt;
    std::string magic;
    std::uint32_t version = 0;
    input >> magic >> version;
    if (magic != "HEARTSTEAD_WORLD" || version != 1U) return std::nullopt;

    WorldSaveData data;
    input >> std::quoted(data.summary.id)
          >> std::quoted(data.summary.name)
          >> std::quoted(data.summary.created)
          >> std::quoted(data.summary.last_played)
          >> data.player_position.x >> data.player_position.y >> data.player_position.z
          >> data.player_yaw >> data.camera_yaw >> data.camera_pitch;
    std::size_t edit_count = 0;
    input >> edit_count;
    if (!input || data.summary.id != id || edit_count > 10'000'000U) return std::nullopt;
    for (std::size_t edit = 0; edit < edit_count; ++edit) {
        Int3 coordinate{};
        std::size_t index = 0;
        std::uint32_t block = 0;
        input >> coordinate.x >> coordinate.y >> coordinate.z >> index >> block;
        if (!input || index >= Chunk::volume || block > std::numeric_limits<BlockId>::max())
            return std::nullopt;
        data.edits.chunks[coordinate][index] = static_cast<BlockId>(block);
    }
    return data;
}

std::vector<WorldSaveSummary> WorldSaveStore::list() const {
    std::vector<WorldSaveSummary> result;
    std::error_code error;
    if (!std::filesystem::exists(root_, error)) return result;
    for (const auto& entry : std::filesystem::directory_iterator(root_, error)) {
        if (error) break;
        const auto regular_file = entry.is_regular_file(error);
        if (error) {
            error.clear();
            continue;
        }
        if (!regular_file || entry.path().extension() != ".hws") continue;
        const auto loaded = load(entry.path().stem().string());
        if (loaded) result.push_back(loaded->summary);
    }
    std::sort(result.begin(), result.end(), [](const auto& lhs, const auto& rhs) {
        return lhs.last_played > rhs.last_played;
    });
    return result;
}

} // namespace heartstead::app
