#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cfloat>
#include <cmath>
#include <mutex>
#include <optional>
#include <imgui.h>
#include <safetyhook.hpp>
#include "key_release.h"
#include "../game.h"
#include "../log.h"

namespace iidxtra::key_release
{
    namespace
    {
        using clock = std::chrono::steady_clock;
        constexpr std::size_t sample_limit = 300;
        constexpr auto hold_limit = std::chrono::milliseconds {200};

        struct key_state
        {
            std::optional<clock::time_point> pressed;
            std::array<double, sample_limit> samples {};
            std::size_t count = 0;
            std::size_t next = 0;
            double total = 0;

            auto add(double milliseconds) -> void
            {
                total -= samples[next];
                samples[next] = milliseconds;
                total += milliseconds;
                next = (next + 1) % sample_limit;
                count = std::min(count + 1, sample_limit);
            }

            auto average() const -> int
            {
                return count ? static_cast<int>(std::lround(total / count)) : 0;
            }
        };

        struct key_position
        {
            float center_x;
            float center_y;
            float width;
        };

        SafetyHookMid position_hook;
        std::atomic_bool installed = false;
        std::mutex mutex;
        bool enabled = false;
        bool in_song = false;
        bool sampled = false;
        std::uint32_t previous_held = 0;
        std::array<key_state, 14> keys;
        std::array<std::optional<key_position>, 14> positions;

        auto record_input_at(std::uint32_t held, clock::time_point now) -> void
        {
            const std::lock_guard lock(mutex);
            if (!enabled || !in_song)
                return;

            // An already-held key at enable time has no known press timestamp.
            if (!sampled)
            {
                previous_held = held;
                sampled = true;
                return;
            }

            for (unsigned index = 0; index < keys.size(); ++index)
            {
                const auto bit = 1u << index;
                if (!((held ^ previous_held) & bit))
                    continue;

                auto& key = keys[index];
                if (held & bit)
                {
                    key.pressed = now;
                    continue;
                }
                if (!key.pressed)
                    continue;

                const auto duration = now - *key.pressed;
                key.pressed.reset();
                if (duration < hold_limit)
                    key.add(std::chrono::duration<double, std::milli>(duration).count());
            }
            previous_held = held;
        }

        auto capture_position(SafetyHookContext& ctx) -> void
        {
            const std::lock_guard lock(mutex);
            if (!enabled || !in_song || !ctx.r14 || !bm2dx::state || bm2dx::state->game_type == 9)
                return;

            const auto& display = *reinterpret_cast<const bm2dx::key_display_t*>(ctx.r14);
            if (display.player >= 2)
            {
                log::print("[Key Release] Unexpected key-display player {}", display.player);
                return;
            }
            const bool active = bm2dx::state->play_style != 0 ?
                bm2dx::state->p1_active || bm2dx::state->p2_active :
                (display.player == 0 ? bm2dx::state->p1_active : bm2dx::state->p2_active);
            if (!active)
                return;

            // Native sprite positioning supplies the live skin anchor, including scratch flip.
            for (unsigned key = 0; key < 7; ++key)
            {
                if (display.lights[key] != reinterpret_cast<void*>(ctx.rcx))
                    continue;

                const auto half_width = static_cast<std::int32_t>(ctx.rbx);
                if (half_width <= 0)
                    return;

                auto* sprite = reinterpret_cast<void*>(ctx.rcx);
                const auto vtable = *static_cast<void***>(sprite);
                int width = 0, height = 0;
                reinterpret_cast<void(*)(void*, int*, int*)>(vtable[39])(sprite, &width, &height);
                if (height <= 0)
                    return;

                positions[display.player * 7 + key] = key_position {
                    static_cast<float>(static_cast<std::int32_t>(ctx.rdx)) + half_width,
                    static_cast<float>(static_cast<std::int32_t>(ctx.r8)) + height * 0.5f,
                    static_cast<float>(half_width * 2)
                };
                return;
            }
        }
    }

    auto available() -> bool
    {
        return installed.load();
    }

    auto set_enabled(bool value) -> void
    {
        const std::lock_guard lock(mutex);
        value = value && available();
        if (enabled == value)
            return;

        enabled = value;
        sampled = false;
        positions = {};
        for (auto& key : keys)
            key.pressed.reset();
    }

    auto reset() -> void
    {
        const std::lock_guard lock(mutex);
        keys = {};
        positions = {};
        sampled = false;
        in_song = false;
    }

    auto set_in_song(bool value) -> void
    {
        const std::lock_guard lock(mutex);
        in_song = value;
        sampled = false;
        positions = {};
        // Keep the session averages, but never join a press across songs or quick retries.
        for (auto& key : keys)
            key.pressed.reset();
    }

    auto record_input(std::uint32_t held) -> void
    {
        record_input_at(held, clock::now());
    }

    auto begin_frame() -> void
    {
        const std::lock_guard lock(mutex);
        positions = {};
    }

    auto render() -> void
    {
        std::array<std::optional<key_position>, 14> layout;
        std::array<int, 14> averages;
        {
            const std::lock_guard lock(mutex);
            layout = positions;
            positions = {};
            if (!enabled || !in_song)
                return;
            for (unsigned index = 0; index < keys.size(); ++index)
                averages[index] = keys[index].average();
        }
        if (!bm2dx::state || bm2dx::state->game_type == 9)
            return;

        const auto screen = ImGui::GetIO().DisplaySize;
        if (screen.x <= 0 || screen.y <= 0)
            return;

        const auto scale_x = screen.x / 1920.0f;
        const auto scale_y = screen.y / 1080.0f;
        auto* draw = ImGui::GetBackgroundDrawList();
        auto* font = ImGui::GetFont();
        for (unsigned index = 0; index < layout.size(); ++index)
        {
            if (!layout[index])
                continue;

            const auto& position = *layout[index];
            const auto text = fmt::format("{}", averages[index]);
            float size = 22.0f * scale_y;
            const auto extent = font->CalcTextSizeA(size, FLT_MAX, 0, text.c_str());
            size *= std::min(1.0f, position.width * 0.90f * scale_x / extent.x);

            // Center the visible digits, excluding font bearings and trailing advance.
            auto* baked = font->GetFontBaked(size);
            const auto glyph_scale = size / baked->Size;
            ImVec2 lower {FLT_MAX, FLT_MAX}, upper {-FLT_MAX, -FLT_MAX};
            float advance = 0;
            for (const unsigned char digit : text)
            {
                const auto* glyph = baked->FindGlyph(digit);
                lower.x = std::min(lower.x, advance + glyph->X0);
                lower.y = std::min(lower.y, glyph->Y0);
                upper.x = std::max(upper.x, advance + glyph->X1);
                upper.y = std::max(upper.y, glyph->Y1);
                advance += glyph->AdvanceX;
            }
            const ImVec2 origin {
                std::round(position.center_x * scale_x - (lower.x + upper.x) * glyph_scale * 0.5f),
                std::round(position.center_y * scale_y - (lower.y + upper.y) * glyph_scale * 0.5f)
            };
            constexpr float outline = 1.0f;
            for (int y = -1; y <= 1; ++y)
                for (int x = -1; x <= 1; ++x)
                    if (x != 0 || y != 0)
                        draw->AddText(font, size, {origin.x + x * outline, origin.y + y * outline},
                            IM_COL32(0, 0, 0, 255), text.c_str());
            draw->AddText(font, size, origin, IM_COL32(255, 255, 255, 255), text.c_str());
        }
    }

    auto shutdown() -> void
    {
        installed = false;
        set_enabled(false);
        position_hook.reset();
    }

    auto install_hook() -> void
    {
        if (!bm2dx::addr->KEY_LIGHT_POSITION)
        {
            log::print("[Key Release] Key positioning is unavailable for this game build");
            return;
        }
        position_hook = safetyhook::create_mid(bm2dx::addr->KEY_LIGHT_POSITION, capture_position);
        installed = static_cast<bool>(position_hook);
        if (!installed)
            log::print("[Key Release] Failed to install key-position hook");
    }
}
