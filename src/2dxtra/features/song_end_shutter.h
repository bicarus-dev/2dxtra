#pragma once

namespace bm2dx { struct play_field_t; struct play_session_t; struct player_timing_t; }

namespace iidxtra::song_end_shutter
{
    extern bool enabled;

    auto initialize(bool draw_hook_available) -> void;
    auto available() -> bool;
    auto in_play() -> bool;
    auto update() -> void;
    auto reset() -> void;
    auto begin_play(const bm2dx::play_field_t* field) -> void;
    auto capture_timing(unsigned player, const bm2dx::player_timing_t& timing) -> void;
    auto end_play() -> void;
    auto draw(bm2dx::play_session_t* session) -> void;
}
