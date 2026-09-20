#include <MinHook.h>
#include <algorithm>
#include <atomic>
#ifdef _MSC_VER
#include <intrin.h>
#endif
#include "../game.h"
#include "../hooks/fast_slow_hook.h"
#include "../util/code_patch.h"
#include "play_visuals.h"

namespace iidxtra::play_visuals
{
    auto dark_mode = false;
    auto no_measure_lines = false;
    auto no_bpm_gradient = false;
    auto concentration_movie = false;
    auto subscreen_dim_level = 0;

    static constexpr auto demo_mode = 9u;
    static constexpr auto subscreen_top = 1080;
    static constexpr auto subscreen_width = 1280;
    static constexpr auto subscreen_height = 720;
    static constexpr auto demonstration_banner_height = 64;

    static auto movie_hooks_available = false;

    // Publish menu settings separately from the game's concentration transitions.
    static auto subscreen_brightness = std::atomic<float> { 1.0f };
    static auto movie_enabled = std::atomic_bool { false };
    static auto concentration_active = std::atomic_bool { false };

    // Only change the normal subscreen UI when entering or leaving concentration.
    static auto subscreen_ui_hidden = false;

    static auto original_game_mode_fn = static_cast<std::uint32_t(*)()>(nullptr);
    static auto original_concentration_show_fn = static_cast<void(*)(void*, bool)>(nullptr);

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
            if (caller == bm2dx::addr->SUBSCREEN_MOVIE_INIT_RETURN)
                return demo_mode;

            // Only the subscreen movie and title checks should see demo mode.
            // Other callers include gameplay logic and main-screen movie positioning.
            if (show_concentration_movie() &&
                (caller == bm2dx::addr->SUBSCREEN_MOVIE_DRAW_RETURN ||
                 caller == bm2dx::addr->SUBSCREEN_TITLE_CALL_RETURN ||
                 caller == bm2dx::addr->SUBSCREEN_TITLE_DRAW_RETURN))
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

    auto draw_concentration_movie() -> void
    {
        if (!movie_hooks_available)
            return;

        // The game creates and destroys this layer between songs; never cache it.
        auto const clip = *reinterpret_cast<void**>(bm2dx::addr->SUBSCREEN_MOVIE_CLIP);
        auto const native_mode = original_game_mode_fn();
        if (clip != nullptr)
        {
            auto const active = show_concentration_movie();
            auto const customize = active && native_mode != demo_mode;

            // Native DP titles start 135 pixels higher. Align the replacement with SP,
            // but restore the native position outside this presentation.
            auto const native_title_y = bm2dx::state->play_style != 0 ? 945 : subscreen_top;
            bm2dx::play_session->subscreen_title_y = customize ? subscreen_top : native_title_y;

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
            using set_color_fn = std::intptr_t(*)(void*, float, float, float, float);
            reinterpret_cast<set_color_fn>(vtable[20])(clip, 1.0f, brightness, brightness, brightness);
        }
    }

    auto install_hook() -> void
    {
        // Per-frame updates reuse the shared sprite hook. Unsupported profiles leave
        // these optional addresses null, which keeps the feature unavailable.
        if (!fast_slow_hook::available() ||
            bm2dx::addr->GET_GAME_MODE_FN == nullptr ||
            bm2dx::addr->CONCENTRATION_SHOW_FN == nullptr ||
            bm2dx::addr->SUBSCREEN_UI_SHOW_FN == nullptr ||
            bm2dx::addr->SUBSCREEN_MOVIE_CLIP == nullptr ||
            bm2dx::addr->SUBSCREEN_MOVIE_INIT_RETURN == nullptr ||
            bm2dx::addr->SUBSCREEN_MOVIE_DRAW_RETURN == nullptr ||
            bm2dx::addr->SUBSCREEN_TITLE_CALL_RETURN == nullptr ||
            bm2dx::addr->SUBSCREEN_TITLE_DRAW_RETURN == nullptr)
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
        }
    }

    auto reset() -> void
    {
        dark_mode = false;
        no_measure_lines = false;
        no_bpm_gradient = false;
        concentration_movie = false;
        subscreen_dim_level = 0;

        update_dark_mode();
        update_no_measure_lines();
        update_no_bpm_gradient();
        update_concentration_movie();
        update_subscreen_dim();
    }

    auto update_dark_mode() -> void
    {
        auto static patch = util::branch_patch { bm2dx::addr->DARK_MODE_PATCH };
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
