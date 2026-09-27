#pragma once

namespace iidxtra::scratch_flip
{
    extern bool enabled_p1;
    extern bool enabled_p2;

    auto install_hook() -> void;
    auto available() -> bool;
    auto update() -> void;
    auto reset() -> void;

    // Native coordinates use a 1920-wide screen; flipped frames move outward to its edges.
    constexpr auto outer_edge_offset(int player) -> int
    {
        return player == 0 ? -42 : player == 1 ? 42 : 0;
    }

    constexpr auto frame_offset(int player) -> int
    {
        return (player == 0 ? -1338 : player == 1 ? 1338 : 0) + outer_edge_offset(player);
    }

    // Move the seven-key block and scratch separately, preserving key order and lane identities.
    constexpr auto lane_offset(int player, int lane) -> int
    {
        if (player < 0 || player > 1 || lane < 0 || lane > 7)
            return 0;

        auto const displacement = lane == 7 ? 342 : -92;
        return (player == 0 ? displacement : -displacement) + outer_edge_offset(player);
    }
}