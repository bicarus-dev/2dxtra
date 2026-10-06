#pragma once

#include "gauge_types.h"

namespace iidxtra::gauge_render
{
    // Snapshot one gauge's presentation before its native sprite calls.
    auto begin_draw(const gauge::player_options& options, int native_gauge, bool tint_easy) -> void;
    auto draw_sprite(void* sprite, const void* caller, int horizontal, int vertical, unsigned layer) -> void;
    auto reset() -> void;
}
