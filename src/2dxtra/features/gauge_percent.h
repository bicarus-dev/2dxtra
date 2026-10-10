#pragma once

#include <string>

namespace iidxtra::gauge_percent
{
    extern bool enabled;

    auto available() -> bool;
    auto status() -> std::string;
    auto update() -> void;
    auto reset() -> void;
    auto init_font() -> void;
    auto render() -> void;
    auto install_hook() -> void;
    auto shutdown() -> void;
}
