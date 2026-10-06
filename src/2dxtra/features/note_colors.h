#pragma once

#include <array>
#include <string>

namespace iidxtra::note_colors
{
    struct column_options
    {
        bool tint_enabled = false;
        std::array<float, 3> tint {1.0f, 1.0f, 1.0f};
        int saturation_percent = 100;
    };

    // P1/left and P2/right, each ordered keys 1-7 then scratch.
    extern std::array<std::array<column_options, 8>, 2> players;

    auto available() -> bool;
    auto status() -> std::string;
    auto update() -> void;
    auto copy_to_other_player(int player) -> void;
    auto reset() -> void;
    auto install_hook() -> void;
    auto shutdown() -> void;
}
