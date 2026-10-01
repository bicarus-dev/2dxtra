#pragma once

namespace iidxtra::audio_balance
{
    constexpr int max_percent = 200;
    extern int keysound_percent;
    extern int bgm_percent;

    auto available() -> bool;
    auto status() -> const char*;
    auto update() -> void;
    auto reset() -> void;
    auto install_hook() -> void;
    auto shutdown() -> void;
}
