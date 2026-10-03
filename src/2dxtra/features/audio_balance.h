#pragma once

namespace iidxtra::audio_balance
{
    // All volume percentages are linear, with 100% preserving native gain.
    constexpr int max_percent = 200;
    constexpr int global_max_percent = 400;
    extern int global_percent;
    extern int keysound_percent;
    extern int bgm_percent;

    auto available() -> bool;
    auto status() -> const char*;
    auto update() -> void;
    auto reset() -> void;
    auto install_hook() -> void;
    auto shutdown() -> void;
}
