#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <d3d9.h>
#include <safetyhook.hpp>
#include "note_colors.h"
#include "color_filter.h"
#include "../game.h"
#include "../log.h"

namespace iidxtra::note_colors
{
    std::array<std::array<column_options, 8>, 2> players;

    namespace
    {
        constexpr int batch_capacity = 512;

        struct vertex
        {
            float x, y, z, rhw;
            std::uint32_t color;
            float u, v;
        };

        struct note_batch
        {
            int count;
            std::byte unused[76];
            vertex vertices[batch_capacity][6];
        };
        static_assert(sizeof(vertex) == 28);
        static_assert(offsetof(note_batch, vertices) == 80);
        static_assert(sizeof(note_batch) == 86096);

        struct column_filters
        {
            color_filter::parameters sprite;
            color_filter::parameters batch;
            bool customized = false;
        };

        SafetyHookInline draw_hook;
        SafetyHookMid sprite_hook;
        SafetyHookMid batch_hook;
        std::atomic_bool installed = false;

        // Settings are converted on the UI thread, then copied by the render thread.
        std::mutex mutex;
        std::array<std::array<column_filters, 8>, 2> requested_filters;
        std::string error;

        // Native sprites retain these pointers until their queued draw is executed.
        thread_local std::array<std::array<column_filters, 8>, 2> render_filters;

        // One filter per queued note, in the native batch's original draw order.
        thread_local std::array<std::array<const color_filter::parameters*, batch_capacity>, 2> batch_filters {};

        // Only sprite creation within the current note draw receives this filter.
        thread_local const color_filter::parameters* current_filter = nullptr;

        auto report(const std::string& message) -> void
        {
            const std::lock_guard lock(mutex);
            if (error == message)
                return;

            error = message;
            log::print("[Note Colors] {}", message);
        }

        auto make_filter(const column_options& options) -> color_filter::parameters
        {
            color_filter::parameters result {0, options.saturation_percent / 100.0f - 1.0f,
                0, true, report};
            if (!options.tint_enabled)
                return result;

            // Replace hue/saturation while retaining the texture's original lightness.
            const auto& rgb = options.tint;
            const float high = std::max({rgb[0], rgb[1], rgb[2]});
            const float low = std::min({rgb[0], rgb[1], rgb[2]});
            const float difference = high - low;
            result.relative = false;
            result.saturation = 0;
            if (difference == 0)
                return result;

            const double sum = static_cast<double>(high) + low;
            const double saturation_range = sum <= 1.0 ? sum : 2.0 - sum;
            result.saturation = static_cast<float>(difference / saturation_range);
            result.saturation *= options.saturation_percent / 100.0f;
            if (high == rgb[0])
                result.hue = 60.0f * ((rgb[1] - rgb[2]) / difference);
            else if (high == rgb[1])
                result.hue = 60.0f * (2.0f + (rgb[2] - rgb[0]) / difference);
            else
                result.hue = 60.0f * (4.0f + (rgb[0] - rgb[1]) / difference);
            if (result.hue < 0)
                result.hue += 360.0f;

            return result;
        }

        auto batches() -> note_batch*
        {
            return reinterpret_cast<note_batch* (*)()>(bm2dx::addr->NOTE_BATCHES)();
        }

        auto color_sprite(SafetyHookContext& ctx) -> void
        {
            if (!current_filter || !ctx.rax)
                return;

            // Later CN/HCN setup changes clipping and blending, not the custom filter.
            color_filter::apply(reinterpret_cast<void*>(ctx.rax), current_filter);
        }

        auto draw_note(void* renderer, int player, std::uint8_t style, int tick,
                       const std::uint32_t* note) -> std::intptr_t
        {
            if (!installed.load() || player < 0 || player >= 2 || !note ||
                (note[0] != 0 && note[0] != 5 && note[0] != 6))
                return draw_hook.call<std::intptr_t>(renderer, player, style, tick, note);

            const auto column = note[4];
            if (column >= 8)
            {
                report("Unexpected note column; native colors used.");
                return draw_hook.call<std::intptr_t>(renderer, player, style, tick, note);
            }

            auto& color = render_filters[player][column];
            {
                const std::lock_guard lock(mutex);
                color = requested_filters[player][column];
            }
            const bool customized = color.customized &&
                bm2dx::state && bm2dx::state->game_type != 9;

            struct context_scope
            {
                const color_filter::parameters* previous;
                ~context_scope() { current_filter = previous; }
            } scope {current_filter};
            current_filter = customized ? &color.sprite : nullptr;

            auto& batch = batches()[player];
            const auto first = batch.count;
            const auto result = draw_hook.call<std::intptr_t>(renderer, player, style, tick, note);
            if (first < 0 || batch.count < first || batch.count > batch_capacity)
            {
                report("Unexpected native note batch size; native batch colors used.");
                batch_filters[player].fill(nullptr);
            }
            else
            {
                for (int index = first; index < batch.count; ++index)
                    batch_filters[player][index] = customized ? &color.batch : nullptr;
            }

            return result;
        }

        auto draw_batch(SafetyHookContext& ctx) -> void
        {
            if (!installed.load())
                return;

            // The renderer's inlined submission loop keeps RBX at batch + 0x1C.
            auto* batch = reinterpret_cast<note_batch*>(ctx.rbx - 0x1C);
            auto* native_batches = batches();

            int player;
            if (batch == &native_batches[0])
                player = 0;
            else if (batch == &native_batches[1])
                player = 1;
            else
            {
                report("Unknown native note batch; native colors used.");
                return;
            }
            if (batch->count < 0 || batch->count > batch_capacity)
            {
                report("Unexpected native note batch size; native colors used.");
                return;
            }

            const auto& colors = batch_filters[player];
            if (std::none_of(colors.begin(), colors.begin() + batch->count,
                            [](const auto* color) { return color != nullptr; }))
                return;

            auto* device = reinterpret_cast<IDirect3DDevice9*>(ctx.rcx);
            HRESULT result = D3D_OK;
            for (int first = 0; first < batch->count;)
            {
                int end = first + 1;
                while (end < batch->count && colors[end] == colors[first])
                    ++end;

                // Keep the native draw order, texture, clipping and vertices.
                const auto* color = colors[first];
                if (color)
                    color_filter::begin_color_filter(device, color);

                result = device->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 2 * (end - first),
                    batch->vertices[first], sizeof(vertex));

                if (color)
                    color_filter::end_color_filter(device, color);
                if (FAILED(result))
                    report("Drawing a colored note batch failed.");
                first = end;
            }

            // The six-byte call has been performed above, in color-compatible groups.
            ctx.rax = static_cast<std::intptr_t>(result);
            ctx.rip = reinterpret_cast<std::uintptr_t>(bm2dx::addr->NOTE_BATCH_DRAW) + 6;
        }
    }

    auto available() -> bool { return installed.load(); }

    auto status() -> std::string
    {
        const std::lock_guard lock(mutex);
        return error;
    }

    auto update() -> void
    {
        const std::lock_guard lock(mutex);
        for (const auto& side : players)
        {
            for (const auto& column : side)
            {
                if (column.saturation_percent < 0 || column.saturation_percent > 100 ||
                    std::any_of(column.tint.begin(), column.tint.end(),
                        [](float value) { return !std::isfinite(value) || value < 0 || value > 1; }))
                {
                    error = "Invalid note color settings; previous settings retained.";
                    log::print("[Note Colors] {}", error);
                    return;
                }
            }
        }

        for (unsigned player = 0; player < players.size(); ++player)
        {
            for (unsigned column = 0; column < players[player].size(); ++column)
            {
                const auto& options = players[player][column];
                auto& color = requested_filters[player][column];
                color.sprite = make_filter(options);
                color.batch = color.sprite;
                color.batch.clear_additive = true;
                color.customized = options.tint_enabled || options.saturation_percent != 100;
            }
        }

        error.clear();
    }

    auto copy_to_other_player(int player) -> void
    {
        if (player < 0 || player >= 2)
        {
            report("Cannot copy note colors: invalid player.");
            return;
        }

        auto& destination = players[1 - player];
        const auto& source = players[player];
        for (int column = 0; column < 7; ++column)
            destination[6 - column] = source[column];

        // Scratch stays scratch rather than participating in the key reversal.
        destination[7] = source[7];
        update();
    }

    auto reset() -> void
    {
        players = {};
        update();
    }

    auto shutdown() -> void
    {
        installed.store(false);
        draw_hook.reset();
        sprite_hook.reset();
        batch_hook.reset();
    }

    auto install_hook() -> void
    {
        const auto& a = *bm2dx::addr;
        if (!a.NOTE_DRAW_FN || !a.NOTE_BATCHES || !a.NOTE_SPRITE_CREATED ||
            !a.NOTE_BATCH_DRAW || !a.BM2D_RENDERER)
        {
            report("Note coloring is unavailable for this game build.");
            return;
        }
        draw_hook = safetyhook::create_inline(a.NOTE_DRAW_FN, draw_note);
        sprite_hook = safetyhook::create_mid(a.NOTE_SPRITE_CREATED, color_sprite);
        batch_hook = safetyhook::create_mid(a.NOTE_BATCH_DRAW, draw_batch);
        if (!draw_hook || !sprite_hook || !batch_hook)
        {
            shutdown();
            report("Could not install note-color hooks.");
            return;
        }
        installed.store(true);
    }
}
