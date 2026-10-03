#include <cstddef>
#include <d3d9.h>
#include "gauge_colors.h"

namespace iidxtra::gauge_colors
{
    namespace
    {
        struct gauge_color
        {
            float hue;
            float saturation;
            float lightness;
            bool relative = false;
        };
        constexpr gauge_color erosion_color {270.0f, 0.8f, 0.0f};
        constexpr gauge_color dan_color {220.0f, 0.9f, 0.0f};
        constexpr gauge_color ex_dan_color {200.0f, 0.45f, 0.4f};
        // Relative mode preserves hue/lightness and scales saturation by (1 + saturation).
        constexpr gauge_color assist_easy_color {0.0f, -0.8f, 0.0f, true};
        constexpr gauge_color easy_color {0.0f, -0.6f, 0.0f, true};
        struct filter_state
        {
            IDirect3DPixelShader9* shader = nullptr;
            float constants[4][4] {};
            bool captured = false;
        };
        thread_local filter_state previous_filter;

        auto end_color_filter(IDirect3DDevice9* device, const gauge_color*) -> void;

        auto begin_color_filter(IDirect3DDevice9* device, const gauge_color* color) -> void
        {
            auto* renderer = reinterpret_cast<std::byte* (*)()>(bm2dx::addr->GAUGE_RENDERER)();
            auto* shader = renderer ? *reinterpret_cast<IDirect3DPixelShader9**>(renderer + 98464) : nullptr;
            if (!shader)
            {
                gauge::report_color_error("Native gauge colorization shader is unavailable.");
                return;
            }
            auto result = device->GetPixelShader(&previous_filter.shader);
            if (SUCCEEDED(result))
                result = device->GetPixelShaderConstantF(1, &previous_filter.constants[0][0], 4);
            if (FAILED(result))
            {
                if (previous_filter.shader)
                    previous_filter.shader->Release();
                previous_filter = {};
                gauge::report_color_error("Cannot capture gauge shader state.");
                return;
            }
            previous_filter.captured = true;
            const float constants[4][4] {
                {color->hue, 0, 0, 0},
                {color->saturation, 0, 0, 0},
                {color->lightness, 0, 0, 0},
                {color->relative ? 1.0f : 0.0f, 0, 0, 0}
            };
            result = device->SetPixelShader(shader);
            if (SUCCEEDED(result))
                result = device->SetPixelShaderConstantF(1, &constants[0][0], 4);
            if (FAILED(result))
            {
                end_color_filter(device, color);
                gauge::report_color_error("Cannot apply gauge colorization shader.");
            }
        }

        auto end_color_filter(IDirect3DDevice9* device, const gauge_color*) -> void
        {
            if (!previous_filter.captured)
                return;
            const auto shader_result = device->SetPixelShader(previous_filter.shader);
            const auto constants_result = device->SetPixelShaderConstantF(
                1, &previous_filter.constants[0][0], 4);
            if (previous_filter.shader)
                previous_filter.shader->Release();
            previous_filter = {};
            if (FAILED(shader_result) || FAILED(constants_result))
                gauge::report_color_error("Cannot restore gauge shader state.");
        }
    }

    auto apply(void* sprite, gauge::type mode, bool assist_easy) -> void
    {
        const auto vtable = *static_cast<void***>(sprite);
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
            case gauge::type::Default:
                color = assist_easy ? &assist_easy_color : &easy_color;
                break;
            default:
                return;
        }

        // The compiled native shader uses c1-c4 separately, unlike filter 8's packed upload.
        using filter_fn = void (*)(IDirect3DDevice9*, const gauge_color*);
        using set_filter_fn = std::intptr_t (*)(void*, filter_fn, filter_fn, const gauge_color*);
        reinterpret_cast<set_filter_fn>(vtable[36])(sprite, begin_color_filter, end_color_filter, color);
    }
}
