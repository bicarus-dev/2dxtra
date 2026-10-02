#pragma once

#include <string>
#include <vector>

namespace iidxtra::custom_audio
{
    enum class channel { music_select, music_decide };

    inline constexpr char follow_bgm[] = "*bgm";

    // Empty selects Default; "*" selects Random; decide also accepts follow_bgm.
    // Otherwise store the scanned filename.
    extern std::string select_file;
    extern std::string decide_file;

    auto files(channel kind) -> const std::vector<std::string>&;
    auto available() -> bool;
    auto status() -> std::string;
    auto update() -> void;
    auto reset() -> void;
    auto enter_select() -> void;
    auto install_hook() -> void;
    auto shutdown() -> void;
}
