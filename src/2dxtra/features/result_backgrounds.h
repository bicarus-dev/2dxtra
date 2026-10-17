#pragma once

#include <array>
#include <string>
#include <vector>

namespace iidxtra::result_backgrounds
{
    enum class pool : unsigned { a, aa, aaa, pass, fail };
    inline constexpr std::array folders {"a", "aa", "aaa", "pass", "fail"};

    // Empty selects native artwork; "*" chooses a random subfolder, then a random PNG within it.
    // Recursively scan 2dxtra_custom\result_img\<pool> beside contents\modules; IFS files are ignored.
    // Otherwise store an immediate subfolder name (e.g. bistrover), never an individual filename.
    // Choose a random PNG within that subfolder per result; root PNGs and empty groups are ignored.
    // PNGs must be 1920x1080. Folder membership determines the pool, not the filename.
    // Selections take effect at the next ordinary song result; courses/dan keep native artwork.
    extern std::array<std::string, folders.size()> selections;

    auto has_subfolder(pool kind, const std::string& name) -> bool;
    auto subfolders(pool kind) -> std::vector<std::string>;
    auto available() -> bool;
    auto status() -> std::string;
    auto update() -> void;
    auto reset() -> void;
    auto enter_result(void* scene) -> void;
    // Observe the native lookup through scratch_flip's shared hook; never allocate another clip.
    auto capture_clip(void* frame, const char* name, void* clip) -> void;
    auto install_hook() -> void;
    // Debug unload must wait for native scene teardown while a custom result is active.
    auto shutdown() -> bool;
}
