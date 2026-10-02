#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <span>
#include "../game.h"

namespace iidxtra::gauge
{
    enum class type { Default, Dan, Erosion };

    struct player_options
    {
        type mode = type::Default;
        int dan_start_percent = 100;
        bool dan_keep = false;
        int erosion_level = 1;
    };

    extern std::array<player_options, 2> players;

    auto available() -> bool;
    auto status() -> std::string;
    auto update() -> void;
    auto reset() -> void;
    auto enter_select() -> void;
    auto finish_play() -> void;
    auto clear_session() -> void;
    auto capture_chart(std::uint8_t player, std::span<const bm2dx::chart_event_t> events) -> void;
    auto blocks_score(std::uint8_t player) -> bool;
    auto install_hook() -> void;
    auto shutdown() -> void;
}
