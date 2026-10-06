#pragma once

#include "../game.h"

namespace iidxtra::live_timing
{
    constexpr int default_y = 200;
    constexpr int max_y = 1068;
    constexpr float range_ms = 200.0f;
    constexpr int bar_spacing = 32;
    enum class mode { Off, Combined, Split };
    extern mode display_mode;
    extern int y_position;
    extern bool flip_left_right;

    auto max_y_position() -> int;
    auto available() -> bool;
    auto status() -> std::string;
    auto update() -> void;
    auto reset() -> void;
    auto begin_play(bool is_dp) -> void;
    auto end_play() -> void;
    auto in_play() -> bool;
    auto is_recording() -> bool;
    auto record_note(int player, bool scratch, int tick, float milliseconds, int display_code,
                     const bm2dx::timing_t& note_windows) -> void;
    auto render() -> void;
}
