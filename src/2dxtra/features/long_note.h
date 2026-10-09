#pragma once

#include <cstdint>
#include <optional>
#include "../game.h"

namespace iidxtra::long_note
{
    auto available() -> bool;
    auto enabled(int player) -> bool;
    struct judgment
    {
        bool suppress;
        int grade;
        int measure;
        std::optional<bm2dx::judge_candidate_t> head;
    };
    auto process_judgment(void* context, int player, int grade, int lane, int measure,
                         bool press, bool release) -> judgment;
    auto set_enabled(std::uint8_t player, bool enabled) -> void;
    auto reset() -> void;
    auto install_hook() -> void;
    auto shutdown() -> void;
}
