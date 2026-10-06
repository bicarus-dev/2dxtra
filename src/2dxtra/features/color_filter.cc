#include <cstddef>
#include <d3d9.h>
#include "color_filter.h"
#include "../game.h"
#include "../log.h"

namespace iidxtra::color_filter
{
    namespace
    {
        struct filter_state
        {
            IDirect3DPixelShader9* shader = nullptr;
            float constants[5][4] {};
            bool captured = false;
        };
        thread_local filter_state previous_filter;

        auto report(const parameters* color, const char* message) -> void
        {
            if (color->report)
                color->report(message);
            else
                log::print("[Color Filter] {}", message);
        }
    }

    auto begin_color_filter(IDirect3DDevice9* device, const parameters* color) -> void
    {
        auto* renderer = reinterpret_cast<std::byte* (*)()>(bm2dx::addr->BM2D_RENDERER)();
        auto* shader = renderer ? *reinterpret_cast<IDirect3DPixelShader9**>(renderer + 98464) : nullptr;
        if (!shader)
        {
            report(color, "Native colorization shader is unavailable.");
            return;
        }

        auto result = device->GetPixelShader(&previous_filter.shader);
        if (SUCCEEDED(result))
            result = device->GetPixelShaderConstantF(0, &previous_filter.constants[0][0], 5);
        if (FAILED(result))
        {
            if (previous_filter.shader)
                previous_filter.shader->Release();
            previous_filter = {};
            report(color, "Cannot capture colorization shader state.");
            return;
        }

        previous_filter.captured = true;
        float constants[5][4] {
            {},
            {color->hue, 0, 0, 0},
            {color->saturation, 0, 0, 0},
            {color->lightness, 0, 0, 0},
            {color->relative ? 1.0f : 0.0f, 0, 0, 0}
        };
        if (!color->clear_additive)
            for (unsigned i = 0; i < 4; ++i)
                constants[0][i] = previous_filter.constants[0][i];

        result = device->SetPixelShader(shader);
        if (SUCCEEDED(result))
            result = device->SetPixelShaderConstantF(0, &constants[0][0], 5);
        if (FAILED(result))
        {
            end_color_filter(device, color);
            report(color, "Cannot apply colorization shader.");
        }
    }

    auto end_color_filter(IDirect3DDevice9* device, const parameters* color) -> void
    {
        if (!previous_filter.captured)
            return;

        const auto shader_result = device->SetPixelShader(previous_filter.shader);
        const auto constants_result = device->SetPixelShaderConstantF(0, &previous_filter.constants[0][0], 5);
        if (previous_filter.shader)
            previous_filter.shader->Release();
        previous_filter = {};
        if (FAILED(shader_result) || FAILED(constants_result))
            report(color, "Cannot restore colorization shader state.");
    }

    auto apply(void* sprite, const parameters* color) -> void
    {
        // Native filter 8 packs its constants incorrectly; use the per-sprite callbacks instead.
        const auto vtable = *static_cast<void***>(sprite);
        using filter_fn = void (*)(IDirect3DDevice9*, const parameters*);
        using set_filter_fn = std::intptr_t (*)(void*, filter_fn, filter_fn, const parameters*);
        reinterpret_cast<set_filter_fn>(vtable[36])(sprite, begin_color_filter, end_color_filter, color);
    }
}
