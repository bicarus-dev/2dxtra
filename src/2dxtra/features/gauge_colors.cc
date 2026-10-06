#include "gauge_colors.h"
#include "color_filter.h"

namespace iidxtra::gauge_colors
{
    namespace
    {
        using gauge_color = color_filter::parameters;
        constexpr gauge_color erosion_color {270.0f, 0.7f, 0.0f, false, gauge::report_color_error};
        constexpr gauge_color dan_color {220.0f, 0.9f, 0.0f, false, gauge::report_color_error};
        constexpr gauge_color ex_dan_color {200.0f, 0.45f, 0.4f, false, gauge::report_color_error};
        constexpr gauge_color hazard_color {0.0f, 0.0f, 0.0f, false, gauge::report_color_error};
        // Relative mode preserves hue/lightness and scales saturation by (1 + saturation).
        constexpr gauge_color assist_easy_color {0.0f, -0.8f, 0.0f, true, gauge::report_color_error};
        constexpr gauge_color easy_color {0.0f, -0.6f, 0.0f, true, gauge::report_color_error};
    }

    auto apply(void* sprite, gauge::type mode, bool assist_easy) -> void
    {
        const gauge_color* color;
        switch (mode)
        {
            case gauge::type::Erosion:
                color = &erosion_color;
                break;
            case gauge::type::Dan:
                color = &dan_color;
                break;
            case gauge::type::ExDan:
                color = &ex_dan_color;
                break;
            case gauge::type::Hazard:
                color = &hazard_color;
                break;
            case gauge::type::Default:
                color = assist_easy ? &assist_easy_color : &easy_color;
                break;
            default:
                return;
        }

        color_filter::apply(sprite, color);
    }
}
