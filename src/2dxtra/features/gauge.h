#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <span>
#include "../game.h"
#include "gauge_types.h"

namespace iidxtra::gauge
{
    // Session-only options, intentionally excluded from database settings.
    extern std::array<player_options, 2> players;

    // Saved visual preference, independent of the session-only gauge rules.
    extern bool tint_easy;

    auto available() -> bool;
    auto custom_available() -> bool;
    auto in_play() -> bool;
    auto status() -> std::string;
    auto update() -> void;
    auto update_tint() -> void;
    auto reset() -> void;
    auto enter_select() -> void;
    auto finish_play() -> void;
    auto clear_session() -> void;
    auto capture_chart(std::uint8_t player, std::span<const bm2dx::chart_event_t> events) -> void;
    // Shared native sprite-draw callback; presentation is delegated to the gauge renderer.
    auto on_sprite_draw(void* sprite, const void* caller, int horizontal, int vertical, unsigned layer) -> void;
    auto report_draw_error(const std::string& message) -> void;
    auto install_hook() -> void;
    auto shutdown() -> void;
}
