#pragma once

#include "gauge.h"

namespace iidxtra::gauge
{
    // Route render errors through the gauge's existing status and log reporting.
    auto report_color_error(const std::string& message) -> void;
}

namespace iidxtra::gauge_colors
{
    // Default is passed only for live Easy/Assist Easy; native creation resets sprite colors.
    auto apply(void* sprite, gauge::type mode, bool assist_easy) -> void;
}
