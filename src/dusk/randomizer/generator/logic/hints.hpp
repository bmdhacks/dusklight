#pragma once

namespace randomizer::logic::hints {

    void GenerateAllHints(world::WorldPool& worldPool);
    void GenerateAllHints(const std::unique_ptr<world::World>& world);

}