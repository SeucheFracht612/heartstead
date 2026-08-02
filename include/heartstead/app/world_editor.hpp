#pragma once

#include "heartstead/core/types.hpp"

namespace heartstead {

class ChunkWorld;
class OpenGlRenderer;

namespace world {
struct WorldEdits;
}

namespace app {

class WorldEditor {
public:
    [[nodiscard]] static bool set_block(
        ChunkWorld& chunk_world,
        world::WorldEdits& edits,
        OpenGlRenderer& renderer,
        Int3 position,
        BlockId block);
};

} // namespace app
} // namespace heartstead
