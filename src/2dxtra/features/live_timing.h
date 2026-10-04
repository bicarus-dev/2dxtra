#pragma once

#include "../game.h"

namespace iidxtra::live_timing
{
    constexpr int default_y = 200;
    constexpr int max_y = 1068;
    constexpr float range_ms = 200.0f;
    extern bool enabled;
    extern int y_position;

    auto available() -> bool;
    auto status() -> std::string;
    auto update() -> void;
    auto reset() -> void;
    auto begin_play(bool is_dp) -> void;
    auto end_play() -> void;
    auto is_recording() -> bool;
    auto record_note(int player, int tick, float milliseconds, int display_code) -> void;
    auto render() -> void;
}
