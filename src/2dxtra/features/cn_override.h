#pragma once

#include <cstdint>

namespace iidxtra::cn_override
{
    enum note_type { ChartDefault, CN, HCN, LN };

    extern int p1_override_type;
    extern int p2_override_type;

    auto active_player(int player) -> bool;
    auto invalidate_score(std::uint8_t player) -> void;
    auto reset() -> void;

    auto set_initial_states(std::uint8_t player) -> void;
    auto update(std::uint8_t player) -> void;
}