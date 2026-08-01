#include "engine/renderer/benchmark/benchmark_coverage.hpp"

#include <array>
#include <cassert>
#include <set>
#include <string_view>

int main() {
    using namespace heartstead::renderer::benchmark;
    constexpr std::array required{
        "terrain",          "forest",      "crop-fields",      "characters",
        "equipment",        "dense-settlement", "active-workshop", "many-local-lights",
        "cave",             "rapid-edits", "large-coordinates", "fast-traversal",
        "streaming",        "water",       "rain",             "fog",
        "night",            "particles",   "transparency",     "animation",
        "resize",
    };
    const auto coverage = renderer_benchmark_coverage();
    assert(coverage.size() == required.size());
    std::set<std::string_view> unique;
    for (const auto requirement : required) {
        const auto* entry = find_renderer_benchmark_coverage(requirement);
        assert(entry != nullptr);
        assert(!entry->exercised_systems.empty());
        assert(!benchmark_scene_name(entry->scene).empty());
        assert(unique.insert(entry->requirement).second);
    }
    assert(find_renderer_benchmark_coverage("not-a-scenario") == nullptr);
    return 0;
}
