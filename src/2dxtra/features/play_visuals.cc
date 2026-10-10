#include <MinHook.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <numbers>
#include <safetyhook.hpp>
#include "../game.h"
#include "../log.h"
#include "../util/code_patch.h"
#include "play_visuals.h"
#include "key_release.h"

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
        return draw_hook_available && key_release::available();
    }

    auto update_io_key_display() -> void
    {
        io_key_display_enabled = io_key_display && io_key_display_available();
        key_release::set_enabled(io_key_display_enabled.load());
    }

    auto capture_key_display_input(const bm2dx::input_t& input) -> void
    {
        // Despite its legacy name, buttons_edge is the native held mask at manager +0x0C.
        const auto buttons = std::uint64_t {input.buttons_edge};
        const auto left = static_cast<std::uint64_t>(input.p1_turntable & 0xFF);
        const auto right = static_cast<std::uint64_t>(input.p2_turntable & 0xFF);
        key_display_input.store(buttons | (left << 32) | (right << 40) | (1ull << 48));
        key_release::record_input(input.buttons_edge);
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
        std::byte unused[0x1390];
        void* movie;
        std::byte unused_1398[0x10];
        void** sprites_begin;
        void** sprites_end;
        std::byte unused_13B8[8];
        void* sub_movie;
    };
    static_assert(offsetof(bga_context, movie) == 0x1390);
    static_assert(offsetof(bga_context, sprites_begin) == 0x13A8);
    static_assert(offsetof(bga_context, sprites_end) == 0x13B0);
    static_assert(offsetof(bga_context, sub_movie) == 0x13C0);

    // Native render-target descriptor; the game handles its remaining texture properties.
    struct movie_texture
    {
        std::uint32_t id;
        int width;
        int height;
        std::byte properties[28];
    };
    static_assert(sizeof(movie_texture) == 40);

    // Publish menu settings separately from the game's concentration transitions.
    static auto subscreen_brightness = std::atomic<float> { 1.0f };
    static auto movie_enabled = std::atomic_bool { false };
    static auto concentration_active = std::atomic_bool { false };

    // Set at song load, not by menu changes: a forced video stays loaded for that song.
    static auto movie_load_forced = std::atomic_bool { false };

    // Only change the normal subscreen UI when entering or leaving concentration.
    static auto subscreen_ui_hidden = false;

    static std::array<SafetyHookMid, 3> movie_mode_hooks;
    static SafetyHookMid bga_source_hook;
    static SafetyHookMid movie_load_hook;
    static auto original_concentration_show_fn = static_cast<void(*)(void*, bool)>(nullptr);
    static auto original_play_scene_draw_fn = static_cast<std::intptr_t(*)(void*)>(nullptr);
    static auto original_bga_update_fn = static_cast<std::intptr_t(*)(bga_context*)>(nullptr);
    static const char* movie_update_error = nullptr;

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

    static auto report_movie_update_error(const char* message) -> void
    {
        if (movie_update_error != message)
            log::print("[Visuals] {}", message);
        movie_update_error = message;
    }

    static auto update_subscreen_movie(bga_context* context) -> void
    {
        if (!movie_hooks_available || !movie_enabled || !concentration_active ||
            bm2dx::state->game_type == demo_mode || !context->movie || !context->sub_movie)
            return;

        // The native update is gated by a main-screen movie element, absent in camera-only layouts.
        const auto& addresses = *bm2dx::addr;
        auto* element = reinterpret_cast<void*(*)(void*, const char*)>(addresses.PLAY_ELEMENT_FIND)(
            context->sub_movie, "movie");
        if (!element)
        {
            report_movie_update_error("Subscreen movie element unavailable; texture update skipped.");
            return;
        }
        const auto* texture = reinterpret_cast<const movie_texture*(*)(void*)>(
            addresses.MOVIE_TEXTURE_FN)(context->movie);
        if (!texture || texture->width <= 0 || texture->height <= 0)
            return; // The render target is not ready.

        int width = 0;
        int height = 0;
        reinterpret_cast<void(*)(void*, int*, int*)>(addresses.ELEMENT_SIZE_FN)(element, &width, &height);
        if (width <= 0 || height <= 0)
        {
            report_movie_update_error("Invalid subscreen movie dimensions; texture update skipped.");
            return;
        }
        movie_texture scaled {};
        reinterpret_cast<movie_texture*(*)(void*, movie_texture*, float, float)>(addresses.MOVIE_TEXTURE_SCALE_FN)(
            context->movie, &scaled, static_cast<float>(width) / texture->width,
            static_cast<float>(height) / texture->height);
        reinterpret_cast<void(*)(void*, const char*, const movie_texture*)>(addresses.ELEMENT_TEXTURE_FN)(
            context->sub_movie, "movie", &scaled);
        movie_update_error = nullptr;
    }

    static auto bga_update_hook_fn(bga_context* context) -> std::intptr_t
    {
        const auto result = original_bga_update_fn(context);
        if (!context || !bm2dx::state)
            return result;

        update_subscreen_movie(context);

        // Apply the native RGB multiplier only to the main movie, never its shared texture.
        const auto brightness = bm2dx::state->game_type == demo_mode ? 1.0f : bga_brightness.load();
        for (auto sprite = context->sprites_begin; sprite != context->sprites_end; ++sprite)
        {
            if (!*sprite)
                continue;
            set_brightness(*sprite, brightness);
        }

        // The native zero-result fallback draws video directly onto the main screen.
        // A video loaded only for the subscreen must not appear behind the cameras,
        // even if the user disables the option after loading the song.
        if (result == 0 && movie_load_forced && context->movie && bm2dx::state->game_type != demo_mode)
            return 1;
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

    static auto movie_init_mode_hook(SafetyHookContext& ctx) -> void
    {
        // Create the movie layer before play, even if concentration starts later.
        if (movie_enabled)
            ctx.rax = demo_mode;
    }

    static auto bga_source_init_hook(SafetyHookContext& ctx) -> void
    {
        // Camera-only layouts disable BGA effects separately from video playback.
        if (movie_enabled)
            ctx.r8 = (ctx.r8 & ~std::uintptr_t{0xff}) | 1;
    }

    static auto movie_load_enabled_hook(SafetyHookContext& ctx) -> void
    {
        // Enable the video loader without changing the layout used by the cameras.
        const bool force = movie_enabled && (ctx.rax & 0xff) == 0;
        movie_load_forced = force;
        if (force)
            ctx.rax |= 1;
    }

    static auto movie_draw_mode_hook(SafetyHookContext& ctx) -> void
    {
        if (show_concentration_movie())
            ctx.rax = demo_mode;
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
        auto const native_mode = reinterpret_cast<std::uint32_t(*)()>(bm2dx::addr->GET_GAME_MODE_FN)();
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
        key_release::begin_frame();
        const auto result = original_play_scene_draw_fn(context);
        draw_io_key_display(static_cast<bm2dx::play_session_t*>(context));
        return result;
    }

    auto install_hook() -> void
    {
        key_release::install_hook();
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
        const auto& addresses = *bm2dx::addr;
        if (!draw_hook_available || !bga_hook_available ||
            !addresses.GET_GAME_MODE_FN ||
            !addresses.CONCENTRATION_SHOW_FN ||
            !addresses.SUBSCREEN_UI_SHOW_FN ||
            !addresses.SUB_MOVIE_CLIP ||
            !addresses.SUB_MOVIE_INIT_RETURN ||
            !addresses.BGA_INIT_FN ||
            !addresses.MOVIE_ENABLED_RETURN ||
            !addresses.PLAY_ELEMENT_FIND ||
            !addresses.ELEMENT_SIZE_FN ||
            !addresses.MOVIE_TEXTURE_FN ||
            !addresses.MOVIE_TEXTURE_SCALE_FN ||
            !addresses.ELEMENT_TEXTURE_FN ||
            !addresses.SUB_TITLE_CALL_RETURN ||
            !addresses.SUB_TITLE_DRAW_RETURN)
            return;

        // Fervidex decodes the getter's original instructions to locate game state.
        // Override only subscreen callers, leaving the shared getter intact.
        const std::array sites {addresses.SUB_MOVIE_INIT_RETURN,
            addresses.SUB_TITLE_CALL_RETURN, addresses.SUB_TITLE_DRAW_RETURN};
        for (std::size_t i = 0; i < sites.size(); ++i)
            movie_mode_hooks[i] = safetyhook::create_mid(sites[i], i == 0 ? movie_init_mode_hook : movie_draw_mode_hook);
        bga_source_hook = safetyhook::create_mid(addresses.BGA_INIT_FN, bga_source_init_hook);
        movie_load_hook = safetyhook::create_mid(addresses.MOVIE_ENABLED_RETURN, movie_load_enabled_hook);
        auto const concentration_result = MH_CreateHook(addresses.CONCENTRATION_SHOW_FN,
            reinterpret_cast<LPVOID>(concentration_show_hook_fn),
            reinterpret_cast<LPVOID*>(&original_concentration_show_fn));
        movie_hooks_available = std::ranges::all_of(movie_mode_hooks, [](const auto& hook) { return bool(hook); }) &&
                                bga_source_hook && movie_load_hook && concentration_result == MH_OK;

        // The shared initialization enables hooks later; remove partial installs first.
        if (!movie_hooks_available)
        {
            for (auto& hook : movie_mode_hooks)
                hook.reset();
            bga_source_hook.reset();
            movie_load_hook.reset();
            if (concentration_result == MH_OK)
                MH_RemoveHook(addresses.CONCENTRATION_SHOW_FN);
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
