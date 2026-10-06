#pragma once

#include <cstdint>

namespace iidxtra::key_release
{
    // Per-key mean of the last 300 in-song releases under 200 ms, retained until profile logout.
    auto install_hook() -> void;
    auto shutdown() -> void;
    auto available() -> bool;
    auto set_enabled(bool enabled) -> void;
    auto reset() -> void;
    auto set_in_song(bool in_song) -> void;
    auto record_input(std::uint32_t held) -> void;
    auto begin_frame() -> void;
    auto render() -> void;
}
