#include <MinHook.h>
#include <algorithm>
#include <atomic>
#include <cstddef>
#include <numbers>
#ifdef _MSC_VER
#include <intrin.h>
#endif
#include "../game.h"
#include "../log.h"
#include "../util/code_patch.h"
#include "play_visuals.h"

namespace iidxtra::play_visuals
{
    auto dark_mode = false;
    auto no_measure_lines = false;
    auto no_bpm_gradient = false;
    auto bga_darkness = 0;
    auto io_key_display = false;
    auto concentration_movie = false;
    auto subscreen_dim_level = 0;

    static constexpr auto demo_mode = 9u;
    static constexpr auto subscreen_top = 1080;
    static constexpr auto subscreen_width = 1280;
    static constexpr auto subscreen_height = 720;
    static constexpr auto demonstration_banner_height = 64;

    static auto movie_hooks_available = false;
    static auto bga_hook_available = false;
    static auto draw_hook_available = false;
    static std::atomic<float> bga_brightness {1.0f};
    static std::atomic_bool io_key_display_enabled {false};

    // Held buttons, two wrapping 8-bit turntable positions, and a valid-sample bit.
    static std::atomic<std::uint64_t> key_display_input {0};

    auto io_key_display_available() -> bool
    {
        return draw_hook_available;
    }

    auto update_io_key_display() -> void
    {
        io_key_display_enabled = io_key_display && draw_hook_available;
    }

    auto capture_key_display_input(const bm2dx::input_t& input) -> void
    {
        // Despite its legacy name, buttons_edge is the native held mask at manager +0x0C.
        const auto buttons = std::uint64_t {input.buttons_edge};
        const auto left = static_cast<std::uint64_t>(input.p1_turntable & 0xFF);
        const auto right = static_cast<std::uint64_t>(input.p2_turntable & 0xFF);
        key_display_input.store(buttons | (left << 32) | (right << 40) | (1ull << 48));
    }

    static auto draw_io_key_display(bm2dx::play_session_t* session) -> void
    {
        if (!io_key_display_enabled || !session || !bm2dx::state ||
            bm2dx::state->game_type == demo_mode)
            return;

        const auto input = key_display_input.load();
        if (!(input & (1ull << 48)))
            return;

        for (unsigned player = 0; player < 2; ++player)
        {
            for (unsigned key = 0; key < 7; ++key)
            {
                auto* light = session->key_displays[player].lights[key];
                if (!light)
                    continue;

                const auto held = (input & (1ull << (player * 7 + key))) != 0;
                const auto vtable = *static_cast<void***>(light);
                reinterpret_cast<void(*)(void*, bool)>(vtable[5])(light, held);
            }

            auto* turntable = session->turntables[player].sprite;
            if (!turntable)
                continue;

            const auto position = (input >> (32 + player * 8)) & 0xFF;
            // Integer 2x scaling stays continuous across the I/O counter's wrap.
            const auto angle = static_cast<float>((position * 2) & 0xFF) *
                (2.0f * std::numbers::pi_v<float> / 256.0f);
            const auto vtable = *static_cast<void***>(turntable);
            reinterpret_cast<void(*)(void*, float)>(vtable[10])(turntable, angle);
        }
    }

    struct bga_context
    {
        std::byte unused[0x13A8];
        void** sprites_begin;
        void** sprites_end;
    };
    static_assert(offsetof(bga_context, sprites_begin) == 629 * sizeof(void*));
    static_assert(offsetof(bga_context, sprites_end) == 630 * sizeof(void*));

    // Publish menu settings separately from the game's concentration transitions.
    static auto subscreen_brightness = std::atomic<float> { 1.0f };
    static auto movie_enabled = std::atomic_bool { false };
    static auto concentration_active = std::atomic_bool { false };

    // Only change the normal subscreen UI when entering or leaving concentration.
    static auto subscreen_ui_hidden = false;

    static auto original_game_mode_fn = static_cast<std::uint32_t(*)()>(nullptr);
    static auto original_concentration_show_fn = static_cast<void(*)(void*, bool)>(nullptr);
    static auto original_play_scene_draw_fn = static_cast<std::intptr_t(*)(void*)>(nullptr);
    static auto original_bga_update_fn = static_cast<std::intptr_t(*)(bga_context*)>(nullptr);

    auto bga_darkness_available() -> bool
    {
        return bga_hook_available;
    }

    auto update_bga_darkness() -> void
    {
        bga_darkness = std::clamp(bga_darkness, 0, 100);
        bga_brightness.store(1.0f - bga_darkness / 100.0f);
    }

    static auto set_brightness(void* sprite, float brightness) -> void
    {
        const auto vtable = *static_cast<void***>(sprite);
        using set_color_fn = std::intptr_t(*)(void*, float, float, float, float);
        reinterpret_cast<set_color_fn>(vtable[20])(sprite, 1.0f, brightness, brightness, brightness);
    }

    static auto bga_update_hook_fn(bga_context* context) -> std::intptr_t
    {
        const auto result = original_bga_update_fn(context);
        if (!context || !bm2dx::state)
            return result;

        // Apply the native RGB multiplier only to the main movie, never its shared texture.
        const auto brightness = bm2dx::state->game_type == demo_mode ? 1.0f : bga_brightness.load();
        for (auto sprite = context->sprites_begin; sprite != context->sprites_end; ++sprite)
        {
            if (!*sprite)
                continue;
            set_brightness(*sprite, brightness);
        }
        return result;
    }

    auto concentration_movie_available() -> bool
    {
        return movie_hooks_available;
    }

    auto update_concentration_movie() -> void
    {
        movie_enabled = concentration_movie && movie_hooks_available;
    }

    static auto show_concentration_movie() -> bool
    {
        return movie_enabled && concentration_active;
    }

    auto update_subscreen_dim() -> void
    {
        // The five slider positions represent 0%, 20%, 40%, 60%, and 80% dimming.
        subscreen_dim_level = std::clamp(subscreen_dim_level, 0, 4);
        subscreen_brightness = 1.0f - static_cast<float>(subscreen_dim_level) * 0.2f;
    }

    static auto game_mode_hook_fn() -> std::uint32_t
    {
#ifdef _MSC_VER
        auto const caller = _ReturnAddress();
#else
        auto const caller = __builtin_return_address(0);
#endif
        if (movie_enabled)
        {
            // Create the demo movie layer before play, even if concentration starts later.
            if (caller == bm2dx::addr->SUB_MOVIE_INIT_RETURN)
                return demo_mode;

            // Only the subscreen movie and title checks should see demo mode.
            // Other callers include gameplay logic and main-screen movie positioning.
            if (show_concentration_movie() &&
                (caller == bm2dx::addr->SUB_MOVIE_DRAW_RETURN ||
                 caller == bm2dx::addr->SUB_TITLE_CALL_RETURN ||
                 caller == bm2dx::addr->SUB_TITLE_DRAW_RETURN))
                return demo_mode;
        }

        return original_game_mode_fn();
    }

    static auto concentration_show_hook_fn(void* context, bool visible) -> void
    {
        // Keep the native toggle and inactivity timer; replace only their presentation.
        concentration_active = visible;
        auto const enabled = movie_enabled.load();
        auto const hide_ui = visible && enabled;
        if (hide_ui != subscreen_ui_hidden)
        {
            reinterpret_cast<void(*)(void*, bool)>(bm2dx::addr->SUBSCREEN_UI_SHOW_FN)(context, !hide_ui);
            subscreen_ui_hidden = hide_ui;
        }

        // The ordinary concentration background would cover the movie.
        original_concentration_show_fn(context, visible && !enabled);
    }

    static auto draw_concentration_movie() -> void
    {
        if (!movie_hooks_available)
            return;

        // The game creates and destroys this layer between songs; never cache it.
        auto const clip = *reinterpret_cast<void**>(bm2dx::addr->SUB_MOVIE_CLIP);
        auto const native_mode = original_game_mode_fn();
        if (clip != nullptr)
        {
            auto const active = show_concentration_movie();
            auto const customize = active && native_mode != demo_mode;

            // Native DP titles start 135 pixels higher. Align the replacement with SP,
            // but restore the native position outside this presentation.
            auto const native_title_y = bm2dx::state->play_style != 0 ? 945 : subscreen_top;
            bm2dx::play_session->SUB_TITLE_y = customize ? subscreen_top : native_title_y;

            // Preserve real attract demos and follow the native concentration state in play.
            auto const visible = native_mode == demo_mode || active;
            auto const vtable = *static_cast<void***>(clip);
            using set_visible_fn = void(*)(void*, bool);
            reinterpret_cast<set_visible_fn>(vtable[5])(clip, visible);

            // Crop the top 64 pixels to hide the "Demonstration" banner.
            // The native rectangle is x/y/width/height, not Windows RECT edges.
            auto const banner_height = customize ? demonstration_banner_height : 0;
            std::int32_t const clip_rect[] {
                0, subscreen_top + banner_height, subscreen_width, subscreen_height - banner_height
            };
            using set_clip_fn = std::intptr_t(*)(void*, const std::int32_t*);
            reinterpret_cast<set_clip_fn>(vtable[15])(clip, clip_rect);

            // Multiply RGB, keeping alpha unchanged so the scene's own fades still work.
            // Restore full brightness for native demos or when the option is disabled.
            auto const brightness = customize ? subscreen_brightness.load() : 1.0f;
            set_brightness(clip, brightness);
        }
    }

    static auto play_scene_draw_hook_fn(void* context) -> std::intptr_t
    {
        // Apply layer state and title positioning before the native scene draws its UI.
        draw_concentration_movie();
        const auto result = original_play_scene_draw_fn(context);
        draw_io_key_display(static_cast<bm2dx::play_session_t*>(context));
        return result;
    }

    auto install_hook() -> void
    {
        if (bm2dx::addr->PLAY_SCENE_DRAW_FN)
        {
            draw_hook_available = MH_CreateHook(bm2dx::addr->PLAY_SCENE_DRAW_FN,
                reinterpret_cast<LPVOID>(play_scene_draw_hook_fn),
                reinterpret_cast<LPVOID*>(&original_play_scene_draw_fn)) == MH_OK;
            if (!draw_hook_available)
                log::print("[Visuals] Failed to install play UI hook");
        }
        else
            log::print("[Visuals] I/O key display is unavailable for this game build");

        if (bm2dx::addr->BGA_UPDATE_FN)
        {
            bga_hook_available = MH_CreateHook(bm2dx::addr->BGA_UPDATE_FN,
                reinterpret_cast<LPVOID>(bga_update_hook_fn),
                reinterpret_cast<LPVOID*>(&original_bga_update_fn)) == MH_OK;
            if (!bga_hook_available)
                log::print("[Visuals] Failed to install BGA brightness hook");
        }

        // Unsupported profiles leave these optional addresses null, keeping the
        // complete subscreen feature unavailable rather than installing partial behavior.
        if (bm2dx::addr->GET_GAME_MODE_FN == nullptr ||
            bm2dx::addr->CONCENTRATION_SHOW_FN == nullptr ||
            !draw_hook_available ||
            bm2dx::addr->SUBSCREEN_UI_SHOW_FN == nullptr ||
            bm2dx::addr->SUB_MOVIE_CLIP == nullptr ||
            bm2dx::addr->SUB_MOVIE_INIT_RETURN == nullptr ||
            bm2dx::addr->SUB_MOVIE_DRAW_RETURN == nullptr ||
            bm2dx::addr->SUB_TITLE_CALL_RETURN == nullptr ||
            bm2dx::addr->SUB_TITLE_DRAW_RETURN == nullptr)
            return;

        auto const mode_result = MH_CreateHook(bm2dx::addr->GET_GAME_MODE_FN,
            reinterpret_cast<LPVOID>(game_mode_hook_fn),
            reinterpret_cast<LPVOID*>(&original_game_mode_fn));
        auto const concentration_result = MH_CreateHook(bm2dx::addr->CONCENTRATION_SHOW_FN,
            reinterpret_cast<LPVOID>(concentration_show_hook_fn),
            reinterpret_cast<LPVOID*>(&original_concentration_show_fn));
        movie_hooks_available = mode_result == MH_OK &&
                                concentration_result == MH_OK;

        // The shared initialization enables hooks later; remove partial installs first.
        if (!movie_hooks_available)
        {
            if (mode_result == MH_OK)
                MH_RemoveHook(bm2dx::addr->GET_GAME_MODE_FN);
            if (concentration_result == MH_OK)
                MH_RemoveHook(bm2dx::addr->CONCENTRATION_SHOW_FN);
            log::print("[Visuals] Failed to install concentration movie hooks");
        }
    }

    auto reset() -> void
    {
        dark_mode = false;
        no_measure_lines = false;
        no_bpm_gradient = false;
        bga_darkness = 0;
        io_key_display = false;
        concentration_movie = false;
        subscreen_dim_level = 0;

        update_dark_mode();
        update_no_measure_lines();
        update_no_bpm_gradient();
        update_bga_darkness();
        update_io_key_display();
        update_concentration_movie();
        update_subscreen_dim();
    }

    auto update_dark_mode() -> void
    {
        auto static patch = util::code_patch { bm2dx::addr->DARK_MODE_PATCH, { 0x90, 0x90 } };
        dark_mode ? patch.enable(): patch.disable();
    }

    auto update_no_measure_lines() -> void
    {
        auto static patch = util::branch_patch { bm2dx::addr->MEASURE_PATCH };
        no_measure_lines ? patch.enable(): patch.disable();
    }

    auto update_no_bpm_gradient() -> void
    {
        auto static patch = util::code_patch { bm2dx::addr->BPM_BAR_PATCH, {
            0xE9,
            static_cast<std::uint8_t>(bm2dx::addr->BPM_BAR_PATCH_JMP >>  0 & 0xFF),
            static_cast<std::uint8_t>(bm2dx::addr->BPM_BAR_PATCH_JMP >>  8 & 0xFF),
            static_cast<std::uint8_t>(bm2dx::addr->BPM_BAR_PATCH_JMP >> 16 & 0xFF),
            static_cast<std::uint8_t>(bm2dx::addr->BPM_BAR_PATCH_JMP >> 24 & 0xFF),
        } };

        no_bpm_gradient ? patch.enable(): patch.disable();
    }
}
