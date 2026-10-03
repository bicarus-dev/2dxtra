#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <span>
#include "../game.h"

namespace iidxtra::gauge
{
    enum class type { Default, Dan, Erosion, ExDan };

    constexpr auto is_dan(type mode) -> bool
    {
        return mode == type::Dan || mode == type::ExDan;
    }

    struct player_options
    {
        type mode = type::Default;
        int dan_start_percent = 100;
        bool dan_keep = false;
        int erosion_level = 1;
    };

    extern std::array<player_options, 2> players;
    extern bool tint_easy;

    auto available() -> bool;
    auto in_play() -> bool;
    auto status() -> std::string;
    auto update() -> void;
    auto reset() -> void;
    auto enter_select() -> void;
    auto finish_play() -> void;
    auto clear_session() -> void;
    auto capture_chart(std::uint8_t player, std::span<const bm2dx::chart_event_t> events) -> void;
    auto blocks_score(std::uint8_t player) -> bool;
    // Color the current gauge; optional Easy/Assist Easy coloring follows live GSM selections.
    auto tint_sprite(void* sprite, const void* caller) -> void;
    auto install_hook() -> void;
    auto shutdown() -> void;
}
