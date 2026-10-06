#pragma once

namespace iidxtra::autoplay
{
    extern bool enabled_p1;
    extern bool enabled_p2;

    auto reset() -> void;
    auto beam_hook_available() -> bool;

    auto install_hook() -> void;
}