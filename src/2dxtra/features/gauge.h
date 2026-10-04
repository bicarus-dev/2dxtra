#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <span>
#include "../game.h"

namespace iidxtra::gauge
{
    enum class type { Default, Dan, Erosion, ExDan, Hazard };

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

    // Session-only options, intentionally excluded from database settings.
    extern std::array<player_options, 2> players;

    // Saved visual preference, independent of the session-only gauge rules.
    extern bool tint_easy;

    auto available() -> bool;
    auto in_play() -> bool;
    auto status() -> std::string;
    auto update() -> void;
    auto update_tint() -> void;
    auto reset() -> void;
    auto enter_select() -> void;
    auto finish_play() -> void;
    auto clear_session() -> void;
    auto capture_chart(std::uint8_t player, std::span<const bm2dx::chart_event_t> events) -> void;
    // Color the current gauge; optional Easy/Assist Easy coloring follows live GSM selections.
    auto tint_sprite(void* sprite, const void* caller) -> void;
    auto install_hook() -> void;
    auto shutdown() -> void;
}
