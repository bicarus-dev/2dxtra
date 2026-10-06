#include <fmt/format.h>
#include "gauge_render.h"
#include "gauge.h"
#include "color_filter.h"

namespace iidxtra::gauge_render
{
    namespace
    {
        using gauge_color = color_filter::parameters;
        constexpr gauge_color erosion_color {270.0f, 0.7f, 0.0f, false, gauge::report_draw_error};
        constexpr gauge_color dan_color {220.0f, 0.9f, 0.0f, false, gauge::report_draw_error};
        constexpr gauge_color ex_dan_color {200.0f, 0.45f, 0.4f, false, gauge::report_draw_error};
        constexpr gauge_color hazard_color {0.0f, 0.0f, 0.0f, false, gauge::report_draw_error};
        constexpr gauge_color lr2_easy_color {120.0f, 0.8f, 0.0f, false, gauge::report_draw_error};
        // Relative saturation preserves the live native Easy/Assist Easy hue and lightness.
        constexpr gauge_color assist_easy_color {0.0f, -0.8f, 0.0f, true, gauge::report_draw_error};
        constexpr gauge_color easy_color {0.0f, -0.6f, 0.0f, true, gauge::report_draw_error};

        constexpr unsigned background_layer = 168;
        constexpr int label_layer = 165;
        constexpr int label_font = 3;
        constexpr int label_offset_y = -12;

        struct draw_context
        {
            const gauge_color* color = nullptr;
            std::string label;
        };
        thread_local draw_context current;

        auto color_for(gauge::palette palette) -> const gauge_color*
        {
            switch (palette)
            {
                case gauge::palette::Dan: return &dan_color;
                case gauge::palette::Erosion: return &erosion_color;
                case gauge::palette::ExDan: return &ex_dan_color;
                case gauge::palette::White: return &hazard_color;
                case gauge::palette::LR2Easy: return &lr2_easy_color;
                case gauge::palette::Native: return nullptr;
            }
            return nullptr;
        }

        auto is_gauge_caller(const void* caller) -> bool
        {
            DWORD64 image_base = 0;
            const auto function = RtlLookupFunctionEntry(reinterpret_cast<DWORD64>(caller), &image_base, nullptr);
            const auto artwork = reinterpret_cast<DWORD64>(bm2dx::addr->GAUGE_ARTWORK);
            return function && image_base + function->BeginAddress <= artwork &&
                artwork < image_base + function->EndAddress;
        }

        auto draw_label(void* sprite, int horizontal, int vertical) -> void
        {
            if (!bm2dx::addr->TEXT_INIT_FN || !bm2dx::addr->TEXT_RENDER_FN)
            {
                gauge::report_draw_error("Custom gauge labels are unavailable for this game build.");
                return;
            }

            const auto vtable = *static_cast<void***>(sprite);
            int width = 0, height = 0;
            reinterpret_cast<void(*)(void*, int*, int*)>(vtable[39])(sprite, &width, &height);
            if (width <= 0)
                return;

            bm2dx::text_props_t properties {};
            reinterpret_cast<bm2dx::text_props_t* (*)(bm2dx::text_props_t*)>(
                bm2dx::addr->TEXT_INIT_FN)(&properties);
            properties.h_align = 1;
            properties.v_align = 1;
            const auto draw = reinterpret_cast<void(*)(int, int, int, int, bm2dx::text_props_t*, const char*)>(
                bm2dx::addr->TEXT_RENDER_FN);
            const int center = horizontal + width / 2;
            const auto border = fmt::format("<color 000000ff><scale 1.0 1.0>{}</scale></color>", current.label);
            const auto label = fmt::format("<color ffffffff><scale 1.0 1.0>{}</scale></color>", current.label);
            for (int y = -1; y <= 1; ++y)
                for (int x = -1; x <= 1; ++x)
                    if (x != 0 || y != 0)
                        draw(label_font, center + x, vertical + label_offset_y + y,
                            label_layer, &properties, border.c_str());
            draw(label_font, center, vertical + label_offset_y, label_layer, &properties, label.c_str());
        }
    }

    auto begin_draw(const gauge::player_options& options, int native_gauge, bool tint_easy) -> void
    {
        reset();
        const auto* definition = gauge::find_definition(options.mode);
        if (!definition)
        {
            gauge::report_draw_error("Unexpected custom gauge type; drawing override omitted.");
            return;
        }

        if (options.mode == gauge::type::Default)
        {
            if (tint_easy)
                current.color = native_gauge == 1 ? &assist_easy_color : native_gauge == 2 ? &easy_color : nullptr;
        }
        else
        {
            current.color = color_for(definition->color);
            current.label = definition->name;
            if (options.mode == gauge::type::Erosion)
                current.label = fmt::format("{} {}", current.label, options.erosion_level);
        }
    }

    auto draw_sprite(void* sprite, const void* caller, int horizontal, int vertical, unsigned layer) -> void
    {
        if (!sprite || (!current.color && current.label.empty()) || !is_gauge_caller(caller))
            return;
        if (current.color)
            color_filter::apply(sprite, current.color);
        // The native background is emitted once per gauge; foreground, pulse and tip must not duplicate its label.
        if (layer == background_layer && !current.label.empty())
            draw_label(sprite, horizontal, vertical);
    }

    auto reset() -> void
    {
        current = {};
    }
}
