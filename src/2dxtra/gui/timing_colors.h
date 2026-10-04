#pragma once

#include <array>
#include <imgui.h>

namespace iidxtra::gui
{
    inline constexpr std::array timing_colors {
        IM_COL32(255, 255, 255, 255),
        IM_COL32(0, 255, 255, 255),
        IM_COL32(255, 255, 0, 255),
        IM_COL32(255, 128, 0, 255)
    };
}
