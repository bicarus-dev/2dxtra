#pragma once

namespace iidxtra::play_visuals
{
    extern bool dark_mode;
    extern bool no_measure_lines;
    extern bool no_bpm_gradient;
    extern bool concentration_movie;
    extern int subscreen_dim_level;

    auto install_hook() -> void;
    auto concentration_movie_available() -> bool;
    auto update_concentration_movie() -> void;
    auto update_subscreen_dim() -> void;
    auto draw_concentration_movie() -> void;
    auto reset() -> void;
    auto update_dark_mode() -> void;
    auto update_no_measure_lines() -> void;
    auto update_no_bpm_gradient() -> void;
}