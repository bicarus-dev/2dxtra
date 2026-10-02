#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <mutex>
#include <optional>
#include <safetyhook.hpp>
#include "gauge.h"
#include "../log.h"
#include "../hooks/score_invalidator_hook.h"

namespace iidxtra::gauge
{
    std::array<player_options, 2> players;

    namespace
    {
        struct option_block
        {
            std::byte unused[0x40];
            int normal_gauge;
            std::byte unused_44[8];
            int special;
            int starting_percent;
            std::byte tail[0x60];
        };
        struct native_options
        {
            std::byte unused[8];
            option_block blocks[3];
        };
        struct gauge_value
        {
            int value;
            int judgments[4];
        };
        static_assert(sizeof(option_block) == 0xB4);
        static_assert(offsetof(option_block, normal_gauge) == 0x40);
        static_assert(offsetof(option_block, special) == 0x4C);
        static_assert(offsetof(option_block, starting_percent) == 0x50);
        static_assert(offsetof(native_options, blocks) == 8);
        static_assert(sizeof(gauge_value) == 20);

        struct play_state
        {
            player_options options;
            double duration_seconds = 0;
            double drain_percent = 0;
            int interval_ticks = 0;
            int next_drain_tick = 0;
            int end_tick = 0;
        };
        struct saved_options
        {
            native_options* object = nullptr;
            unsigned block = 0;
            int special = 0;
            int starting_percent = 0;
        };

        SafetyHookInline reset_hook, coefficients_hook, judgment_hook, tick_hook;
        SafetyHookMid artwork_hook;
        std::atomic_bool installed = false;
        std::array<std::atomic_bool, 2> score_blocked {};
        std::mutex mutex;
        std::array<player_options, 2> requested;
        std::array<std::optional<int>, 2> carried_percent;
        std::array<play_state, 2> playing;
        std::array<saved_options, 2> saved;
        std::string error;
        bool double_play = false;
        unsigned dp_player = 0;
        bool finished = false;

        struct erosion_parameters
        {
            std::array<int, 6> judgment_deltas;
            int interval_seconds;
            double drain_multiplier;
        };
        constexpr std::array<erosion_parameters, 5> erosion_levels {{
            {{32, 32, 8, -200, -400, -200}, 5, 3.5},
            {{32, 24, 4, -300, -600, -300}, 5, 4.5},
            {{32, 16, 0, -500, -1000, -500}, 3, 4.0},
            {{32, 0, 0, -600, -1200, -600}, 3, 5.0},
            {{32, -16, -600, -800, -1200, -800}, 3, 6.0}
        }};
        constexpr std::array<int, 6> ex_dan_judgments {8, 8, 2, -170, -250, -170};

        auto remove_hooks() -> void
        {
            reset_hook.reset();
            coefficients_hook.reset();
            judgment_hook.reset();
            tick_hook.reset();
            artwork_hook.reset();
        }

        auto options() -> native_options*
        {
            return reinterpret_cast<native_options* (*)()>(bm2dx::addr->GET_PLAYER_OPTIONS)();
        }

        auto values() -> gauge_value*
        {
            return reinterpret_cast<gauge_value*>(bm2dx::addr->GAUGE_VALUES);
        }

        auto report(const std::string& message) -> void
        {
            const std::lock_guard lock(mutex);
            error = message;
            log::print("[Gauge] {}", message);
        }

        auto restore_options() -> void
        {
            const auto current = options();
            for (auto& previous : saved)
            {
                // Card-in can replace the options object. Never write through a stale pointer.
                if (previous.object && current == previous.object)
                {
                    auto& block = current->blocks[previous.block];
                    block.special = previous.special;
                    block.starting_percent = previous.starting_percent;
                }
                previous = {};
            }
        }

        auto invalidate() -> void
        {
            for (std::uint8_t player = 0; player < 2; ++player)
                if (score_blocked[player].load())
                    score_invalidator_hook::invalidate(player);
        }

        auto suspend_gsm() -> bool
        {
            const auto module = GetModuleHandleW(L"2dx-gsm.dll");
            if (!module)
                return true;
            const auto enabled = reinterpret_cast<bool (*)()>(GetProcAddress(module, "is_enabled"));
            // GSM's query also restores its failure-suppression patch on entering Dan practice.
            if (enabled && !enabled())
                return true;
            report("Installed 2dx-gsm did not suspend; special gauges are disabled for this play.");
            return false;
        }

        auto disable_play() -> void
        {
            restore_options();
            playing = {};
            // A failed attempt to enable a special gauge must remain unsaveable.
        }

        auto reset_native() -> std::intptr_t
        {
            // Quick retry can skip the result screen after a failure.
            {
                const std::lock_guard lock(mutex);
                for (unsigned player = 0; player < 2; ++player)
                    if (is_dan(playing[player].options.mode) &&
                        playing[player].options.dan_keep && values()[player].value <= 0)
                        carried_percent[player] = 100;
            }
            restore_options();
            playing = {};
            for (auto& blocked : score_blocked)
                blocked.store(false);
            finished = false;
            const auto state = bm2dx::state;
            if (!state)
                return reset_hook.call<std::intptr_t>();
            double_play = state->play_style == static_cast<int>(bm2dx::play_style::DP);
            dp_player = state->p1_active ? 0u : 1u;
            const auto current = options();
            std::array<player_options, 2> selection;
            std::array<std::optional<int>, 2> carry;
            {
                const std::lock_guard lock(mutex);
                selection = requested;
                carry = carried_percent;
                error.clear();
            }
            bool special = false;
            for (unsigned player = 0; player < 2; ++player)
            {
                const bool active = player == 0 ? state->p1_active != 0 : state->p2_active != 0;
                if (!active || (double_play && player != dp_player) ||
                    selection[player].mode == type::Default)
                    continue;
                score_blocked[player].store(true);
                special = true;
            }
            if (special && double_play)
            {
                score_blocked[0].store(true);
                score_blocked[1].store(true);
            }
            if (special)
            {
                // Leave native courses, Hazard, Arena and demos in control of their own gauges.
                const auto mode = state->game_type;
                if (!current || (mode != 1 && mode != 2 && mode != 5 && mode != 6))
                {
                    report("Special gauges are only available in ordinary Free, Standard, Step Up and Premium Free play.");
                }
                else
                {
                    for (unsigned player = 0; player < 2; ++player)
                    {
                        if (!score_blocked[player].load() || (double_play && player != dp_player))
                            continue;
                        auto& run = playing[player];
                        run.options = selection[player];
                        const auto block_index = double_play ? 2u : player;
                        auto& block = current->blocks[block_index];
                        saved[player] = {current, block_index, block.special, block.starting_percent};
                        block.special = 1;
                        block.starting_percent = 100;
                        if (is_dan(run.options.mode))
                            block.starting_percent = run.options.dan_keep ?
                                carry[player].value_or(run.options.dan_start_percent) : run.options.dan_start_percent;
                    }
                    if (!suspend_gsm())
                        disable_play();
                }
                invalidate();
            }
            const auto result = reset_hook.call<std::intptr_t>();
            if (special)
            {
                const auto native_special = reinterpret_cast<void* const*>(bm2dx::addr->GAUGE_SPECIAL_STATE);
                for (unsigned player = 0; player < 2; ++player)
                    if (playing[player].options.mode != type::Default && native_special[2 * player])
                    {
                        report("A native event gauge is active; special gauge override was disabled.");
                        disable_play();
                        return reset_hook.call<std::intptr_t>();
                    }
            }
            return result;
        }

        auto density_factor(float density, bool dp) -> float
        {
            // INFINITAS adjusts erosion using note density, with separate SP/DP breakpoints.
            const auto thresholds = dp ? std::array {10.4f, 15.4f, 17.7f, 20.0f} :
                std::array {10.2f, 15.3f, 17.9f, 20.5f};
            if (density <= thresholds[0])
                return std::clamp(density / thresholds[0], 0.0f, 1.0f);
            if (density <= thresholds[1])
                return 1.0f + (density - thresholds[0]) / (thresholds[1] - thresholds[0]) * 0.5f;
            if (density <= thresholds[2])
                return 1.5f + (density - thresholds[1]) / (thresholds[2] - thresholds[1]) * 0.25f;
            return 1.75f + std::clamp((density - thresholds[2]) /
                (thresholds[3] - thresholds[2]), 0.0f, 1.0f) * 0.25f;
        }

        auto calculate_hazard_coefficients(unsigned player, int notes) -> void
        {
            auto& block = options()->blocks[double_play ? 2 : player];
            const auto special = block.special;
            const auto normal_gauge = block.normal_gauge;
            block.special = 0;
            block.normal_gauge = 0;

            // Query native Normal coefficients without invoking GSM's simulation.
            const auto calculate = reinterpret_cast<int (*)(unsigned, int, int)>(
                bm2dx::addr->GAUGE_INDIVIDUAL_COEFFICIENT);
            auto& judgments = values()[player].judgments;
            for (int kind = 0; kind < 4; ++kind)
                judgments[kind] = calculate(player, kind, notes);

            block.special = special;
            block.normal_gauge = normal_gauge;
        }

        auto calculate_coefficients(int p1_notes, int p2_notes) -> std::intptr_t
        {
            const auto result = coefficients_hook.call<std::intptr_t>(p1_notes, p2_notes);
            const auto tick_ms = reinterpret_cast<float (*)()>(bm2dx::addr->GAUGE_TICK_MS)();
            for (unsigned player = 0; player < 2; ++player)
            {
                auto& run = playing[player];
                if (run.options.mode == type::Hazard)
                {
                    calculate_hazard_coefficients(player, player == 0 ? p1_notes : p2_notes);
                    continue;
                }
                if (run.options.mode != type::Erosion)
                    continue;
                const auto notes = double_play ? static_cast<double>(p1_notes) + p2_notes :
                    static_cast<double>(player == 0 ? p1_notes : p2_notes);
                if (run.duration_seconds <= 0 || notes <= 0 || !std::isfinite(tick_ms) || tick_ms <= 0)
                {
                    report("Cannot determine erosion chart timing; special gauges are disabled for this play.");
                    disable_play();
                    reset_hook.call<std::intptr_t>();
                    return coefficients_hook.call<std::intptr_t>(p1_notes, p2_notes);
                }
                const auto& level = erosion_levels[run.options.erosion_level - 1];
                const auto density = static_cast<float>(notes / run.duration_seconds);
                const int factor = static_cast<int>(density_factor(density, double_play) * 10000.0f);
                const auto base_drain = (notes * 8.0 * static_cast<double>(0.16f) / 9.0 + 50.0) /
                    (notes * 1000.0);
                run.drain_percent = factor * (base_drain * level.drain_multiplier);
                run.interval_ticks = std::max(1, static_cast<int>(level.interval_seconds * 1000.0 / tick_ms + 0.5));
                run.next_drain_tick = run.interval_ticks;
                run.end_tick = static_cast<int>(run.duration_seconds * 1000.0 / tick_ms);
            }
            invalidate();
            return result;
        }

        auto apply_delta(unsigned player, int delta) -> void
        {
            reinterpret_cast<std::intptr_t (*)(unsigned, int)>(bm2dx::addr->GAUGE_APPLY_DELTA)(player, delta);
        }

        auto judgment(int player, int judge) -> void
        {
            if (player < 0 || player >= 2)
            {
                judgment_hook.call<void>(player, judge);
                return;
            }

            const auto& run = playing[player];
            switch (run.options.mode)
            {
                case type::Hazard:
                    // Empty POOR (5) retains native damage rather than causing instant failure.
                    if (judge == 3 || judge == 4)
                    {
                        apply_delta(player, -values()[player].value);
                        return;
                    }
                    break;
                case type::ExDan:
                case type::Erosion:
                {
                    if (judge < 0 || judge >= 6)
                    {
                        report("Unexpected special-gauge judgment; native gauge update used.");
                        break;
                    }
                    // Preserve the GSM bypass, but apply these rules without native Dan reduction.
                    const auto& deltas = run.options.mode == type::ExDan ? ex_dan_judgments :
                        erosion_levels[run.options.erosion_level - 1].judgment_deltas;
                    apply_delta(player, deltas[judge]);
                    return;
                }
                default:
                    break;
            }
            judgment_hook.call<void>(player, judge);
        }

        auto tick(int frame) -> std::intptr_t
        {
            const auto result = tick_hook.call<std::intptr_t>(frame);
            if (finished)
                return result;
            for (unsigned player = 0; player < 2; ++player)
            {
                auto& run = playing[player];
                if (run.options.mode != type::Erosion || run.interval_ticks <= 0)
                    continue;
                while (run.next_drain_tick <= frame && run.next_drain_tick <= run.end_tick)
                {
                    const auto drain = values()[player].value < 1666 ?
                        run.drain_percent * 0.5 : run.drain_percent;
                    apply_delta(player, -static_cast<int>(drain * 50.0));
                    run.next_drain_tick += run.interval_ticks;
                }
            }
            return result;
        }

        auto select_artwork(SafetyHookContext& ctx) -> void
        {
            const auto player = static_cast<unsigned>(ctx.rsi);
            if (player >= playing.size())
                return;
            switch (playing[player].options.mode)
            {
                // Renderer-local indices select the bar, pulse and tip; gauge rules stay unchanged.
                case type::Erosion:
                case type::ExDan:
                case type::Hazard:
                    ctx.rbp = 3;
                    ctx.r15 = 3;
                    break;
                default:
                    break;
            }
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
        for (unsigned player = 0; player < 2; ++player)
        {
            auto& value = players[player];
            value.dan_start_percent = std::clamp(value.dan_start_percent, 2, 100) / 2 * 2;
            value.erosion_level = std::clamp(value.erosion_level, 1, 5);
            if (requested[player].mode != value.mode ||
                requested[player].dan_start_percent != value.dan_start_percent ||
                requested[player].dan_keep != value.dan_keep || !value.dan_keep)
                carried_percent[player].reset();
        }
        requested = players;
    }

    auto enter_select() -> void
    {
        if (!available())
            return;
        restore_options();
        playing = {};
        finished = true;
    }

    auto finish_play() -> void
    {
        if (!available() || finished)
            return;
        invalidate();
        const std::lock_guard lock(mutex);
        for (unsigned player = 0; player < 2; ++player)
            if (is_dan(playing[player].options.mode) && playing[player].options.dan_keep)
            {
                const auto value = values()[player].value;
                carried_percent[player] = value <= 0 ? 100 : std::clamp(2 * (value / 100), 2, 100);
            }
        finished = true;
    }

    auto clear_session() -> void
    {
        enter_select();
        const std::lock_guard lock(mutex);
        carried_percent = {};
        for (auto& blocked : score_blocked)
            blocked.store(false);
    }

    auto reset() -> void
    {
        clear_session();
        players = {};
        update();
    }

    auto capture_chart(std::uint8_t player, std::span<const bm2dx::chart_event_t> events) -> void
    {
        if (player >= 2)
            return;
        auto& run = playing[double_play ? dp_player : player];
        for (const auto& event : events)
            if (event.type == bm2dx::chart_event_type::END_OF_SONG)
            {
                // Raw chart events are in milliseconds; native playback ticks are separate.
                run.duration_seconds = std::max(run.duration_seconds, event.offset / 1000.0);
                return;
            }
    }

    auto blocks_score(std::uint8_t player) -> bool
    {
        return player < 2 && score_blocked[player].load();
    }

    auto install_hook() -> void
    {
        if (!bm2dx::addr->GAUGE_RESET || !bm2dx::addr->GAUGE_COEFFICIENTS ||
            !bm2dx::addr->GAUGE_JUDGMENT || !bm2dx::addr->GAUGE_TICK ||
            !bm2dx::addr->GAUGE_APPLY_DELTA || !bm2dx::addr->GAUGE_TICK_MS ||
            !bm2dx::addr->GAUGE_VALUES || !bm2dx::addr->GAUGE_SPECIAL_STATE ||
            !bm2dx::addr->GAUGE_ARTWORK || !bm2dx::addr->GAUGE_INDIVIDUAL_COEFFICIENT)
        {
            report("Special gauges are unavailable for this game build.");
            return;
        }
        reset_hook = safetyhook::create_inline(bm2dx::addr->GAUGE_RESET, reset_native);
        coefficients_hook = safetyhook::create_inline(bm2dx::addr->GAUGE_COEFFICIENTS, calculate_coefficients);
        judgment_hook = safetyhook::create_inline(bm2dx::addr->GAUGE_JUDGMENT, judgment);
        tick_hook = safetyhook::create_inline(bm2dx::addr->GAUGE_TICK, tick);
        artwork_hook = safetyhook::create_mid(bm2dx::addr->GAUGE_ARTWORK, select_artwork);
        if (!reset_hook || !coefficients_hook || !judgment_hook || !tick_hook || !artwork_hook)
        {
            remove_hooks();
            report("Failed to install special gauge hooks.");
            return;
        }
        installed.store(true);
    }

    auto shutdown() -> void
    {
        clear_session();
        installed.store(false);
        remove_hooks();
    }
}
