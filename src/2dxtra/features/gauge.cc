#include <algorithm>
#include <atomic>
#include <cstddef>
#include <limits>
#include <mutex>
#include <optional>
#include <safetyhook.hpp>
#include "gauge.h"
#include "gauge_render.h"
#include "gauge_rules.h"
#include "../log.h"

namespace iidxtra::gauge
{
    // Editable settings for the next attempt.
    std::array<player_options, 2> players;

    // Saved menu preference for Easy/Assist Easy coloring.
    bool tint_easy = false;

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
            int delta_magnitudes[4];
        };
        static_assert(sizeof(option_block) == 0xB4);
        static_assert(offsetof(option_block, normal_gauge) == 0x40);
        static_assert(offsetof(option_block, special) == 0x4C);
        static_assert(offsetof(option_block, starting_percent) == 0x50);
        static_assert(offsetof(native_options, blocks) == 8);
        static_assert(sizeof(gauge_value) == 20);
        static_assert(offsetof(gauge_value, delta_magnitudes) == 4);

        struct saved_options
        {
            // Original object; rejects stale pointers after card-in.
            native_options* object = nullptr;

            // Modified native block: P1 SP, P2 SP, or shared DP.
            unsigned block = 0;

            // Native gauge option to restore.
            int normal_gauge = 0;

            // Native special-gauge option to restore.
            int special = 0;

            // Native starting percentage to restore.
            int starting_percent = 0;

        };

        struct erosion_state
        {
            // Chart length used to calculate erosion density.
            double duration_seconds = 0;

            // Gauge percentage lost at each erosion interval.
            double drain_percent = 0;

            // Native ticks between erosion drains.
            int interval_ticks = 0;

            // Playback tick scheduled for the next drain.
            int next_drain_tick = 0;

            // Last playback tick eligible for erosion drain.
            int end_tick = 0;

        };
        struct attempt_state
        {
            // Retained attempt settings, including its lamp policy.
            player_options options;

            // Setup/fallback failure: use native gauge behavior and scoring.
            bool failed = false;

            // Native options to restore when the override ends.
            saved_options saved;

            // Chart timing and periodic drain progress for Erosion.
            erosion_state erosion;

            std::optional<gauge_rules::lr2_parameters> lr2;

            // Fractional native units carried between LR2 judgments; 50 units = 1%.
            double lr2_fraction = 0;
        };

        enum class phase { idle, playing, results };

        // Initializes each attempt.
        SafetyHookInline reset_hook;

        // Prepares chart-dependent judgment amounts and erosion timing.
        SafetyHookInline deltas_hook;

        // Applies special-gauge judgment damage/recovery.
        SafetyHookInline judgment_hook;

        // Runs periodic erosion drain.
        SafetyHookInline tick_hook;

        // Converts special-gauge lamps to NO PLAY, except FC.
        SafetyHookInline result_clear_hook;

        // Captures presentation state before native gauge drawing.
        SafetyHookMid artwork_hook;

        // Hook installation completed successfully.
        std::atomic_bool installed = false;

        // Render-thread copy of the tint preference.
        std::atomic_bool tint_easy_enabled = false;

        // Protects settings publication and retained attempt metadata.
        std::mutex mutex;

        // Synchronized menu settings for the next reset.
        std::array<player_options, 2> requested;

        // Dan KEEP starting values.
        std::array<std::optional<int>, 2> carried_percent;

        // Attempt state retained until the next attempt/session reset.
        std::array<attempt_state, 2> attempts;

        // Latest message displayed in the Gauge tab.
        std::string error;

        // This attempt uses a shared DP gauge.
        bool double_play = false;

        // Active owner of the shared DP gauge.
        unsigned dp_player = 0;

        // Separates active overrides from retained results.
        std::atomic<phase> current_phase = phase::idle;

        auto remove_hooks() -> void
        {
            reset_hook.reset();
            deltas_hook.reset();
            judgment_hook.reset();
            tick_hook.reset();
            artwork_hook.reset();
            result_clear_hook.reset();
            gauge_render::reset();
        }

        auto options() -> native_options*
        {
            return reinterpret_cast<native_options* (*)()>(bm2dx::addr->GET_PLAYER_OPTIONS)();
        }

        auto values() -> gauge_value*
        {
            return reinterpret_cast<gauge_value*>(bm2dx::addr->GAUGE_VALUES);
        }

        auto read_native_deltas(unsigned player) -> gauge_rules::judgment_deltas
        {
            // Expand the game's four amounts into the same six signed deltas used by our rules.
            const auto& native = values()[player].delta_magnitudes;
            return {native[0], native[0], native[1], -native[2], -native[3], -native[2]};
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
            for (auto& attempt : attempts)
            {
                auto& previous = attempt.saved;
                // Card-in can replace the options object. Never write through a stale pointer.
                if (previous.object && current == previous.object)
                {
                    auto& block = current->blocks[previous.block];
                    block.normal_gauge = previous.normal_gauge;
                    block.special = previous.special;
                    block.starting_percent = previous.starting_percent;
                }
                previous = {};
            }
        }

        auto active_options(unsigned player) -> player_options
        {
            const std::lock_guard lock(mutex);
            if (player >= attempts.size() || current_phase == phase::idle || attempts[player].failed)
                return {};

            return attempts[player].options;
        }

        auto active_mode(unsigned player) -> type
        {
            return active_options(player).mode;
        }

        constexpr auto gsm_warning = "Custom gauges are unavailable while 2dx-gsm.dll is loaded. "
            "Remove GSM and restart the game to use them.";

        auto disable_play() -> void
        {
            // Rejected setup falls back to the native gauge and lamp rules.
            {
                const std::lock_guard lock(mutex);
                for (auto& run : attempts)
                    if (run.options.mode != type::Default)
                        run.failed = true;
            }
            restore_options();
        }

        auto select_players(const bm2dx::state_t& state,
                            const std::array<player_options, 2>& selection) -> std::array<bool, 2>
        {
            std::array<bool, 2> selected {};
            for (unsigned player = 0; player < 2; ++player)
            {
                const bool active = player == 0 ? state.p1_active != 0 : state.p2_active != 0;
                if (!active || (double_play && player != dp_player))
                    continue;

                selected[player] = selection[player].mode != type::Default;
            }
            return selected;
        }

        auto apply_options(native_options& current, const std::array<bool, 2>& selected,
                           const std::array<player_options, 2>& selection,
                           const std::array<std::optional<int>, 2>& carry) -> void
        {
            for (unsigned player = 0; player < 2; ++player)
            {
                if (!selected[player])
                    continue;

                auto& attempt = attempts[player];
                const auto* definition = find_definition(selection[player].mode);
                if (!definition)
                {
                    report("Unexpected gauge type; native gauge used.");
                    attempt.failed = true;
                    continue;
                }
                {
                    const std::lock_guard lock(mutex);
                    attempt.options = selection[player];
                }

                const auto block_index = double_play ? 2u : player;
                auto& block = current.blocks[block_index];
                attempt.saved = {&current, block_index, block.normal_gauge, block.special, block.starting_percent};

                // Use native clear/failure rules without Dan practice's shared clear-mode flag.
                block.special = 0;
                block.normal_gauge = static_cast<int>(definition->base);
                block.starting_percent = definition->starting_percent;
                if (is_dan(attempt.options.mode))
                {
                    block.starting_percent = attempt.options.dan_start_percent;
                    if (attempt.options.dan_keep)
                        block.starting_percent = carry[player].value_or(block.starting_percent);
                }
            }
        }

        auto reset_native() -> std::intptr_t
        {
            // Quick retry can skip the result screen after a failure.
            {
                const std::lock_guard lock(mutex);
                for (unsigned player = 0; player < 2; ++player)
                    if (current_phase == phase::playing && !attempts[player].failed &&
                        is_dan(attempts[player].options.mode) &&
                        attempts[player].options.dan_keep && values()[player].value <= 0)
                        carried_percent[player] = 100;
            }
            restore_options();
            const auto state = bm2dx::state;
            std::array<player_options, 2> selection;
            std::array<std::optional<int>, 2> carry;
            {
                const std::lock_guard lock(mutex);
                attempts = {};
                current_phase = phase::playing;
                double_play = state && state->play_style == static_cast<int>(bm2dx::play_style::DP);
                dp_player = state && state->p1_active ? 0u : 1u;
                selection = requested;
                carry = carried_percent;
                error.clear();
            }
            if (!state)
                return reset_hook.call<std::intptr_t>();
            const auto current = options();
            const auto selected = select_players(*state, selection);
            const bool special = selected[0] || selected[1];
            if (special)
            {
                const bool gsm_loaded = GetModuleHandleW(L"2dx-gsm.dll") != nullptr;
                // Leave native courses, Hazard, Arena and demos in control of their own gauges.
                const auto mode = state->game_type;
                if (gsm_loaded || !current || (mode != 1 && mode != 2 && mode != 5 && mode != 6))
                {
                    report(gsm_loaded ? gsm_warning :
                        "Special gauges are only available in ordinary Free, Standard, Step Up and Premium Free play.");
                    const std::lock_guard lock(mutex);
                    for (unsigned player = 0; player < 2; ++player)
                        attempts[player].failed = selected[player];
                }
                else
                {
                    apply_options(*current, selected, selection, carry);
                }
            }
            const auto result = reset_hook.call<std::intptr_t>();
            if (special)
            {
                const auto native_special = reinterpret_cast<void* const*>(bm2dx::addr->GAUGE_SPECIAL_STATE);
                for (unsigned player = 0; player < 2; ++player)
                    if (active_mode(player) != type::Default && native_special[2 * player])
                    {
                        report("A native event gauge is active; special gauge override was disabled.");
                        disable_play();
                        return reset_hook.call<std::intptr_t>();
                    }
            }
            for (unsigned player = 0; player < 2; ++player)
                if (active_mode(player) != type::Default)
                {
                    const int initial = current->blocks[double_play ? 2 : player].starting_percent * 50;
                    // Native Normal/Easy starts at 22%; custom gauges also have their own start/KEEP.
                    reinterpret_cast<std::intptr_t (*)(unsigned, int)>(bm2dx::addr->GAUGE_APPLY_DELTA)(
                        player, initial - values()[player].value);
                }
            return result;
        }

        auto calculate_native_deltas(unsigned player, int notes, bool dan) -> void
        {
            auto& block = options()->blocks[double_play ? 2 : player];
            const auto special = block.special;
            const auto normal_gauge = block.normal_gauge;
            block.special = dan ? 1 : 0;
            block.normal_gauge = 0;

            // Query Dan or Normal only for these amounts; restore flags before gameplay resumes.
            const auto calculate = reinterpret_cast<int (*)(unsigned, int, int)>(
                bm2dx::addr->GAUGE_DELTA_MAGNITUDE);
            for (int kind = 0; kind < 4; ++kind)
                values()[player].delta_magnitudes[kind] = calculate(player, kind, notes);

            block.special = special;
            block.normal_gauge = normal_gauge;
        }

        auto calculate_deltas(int p1_notes, int p2_notes) -> std::intptr_t
        {
            // Native delta amounts use raw gauge units: 5000 = 100%; 50 = 1 percentage point.
            // [0] Recovery added for PGREAT or GREAT.
            // [1] Recovery added for GOOD.
            // [2] Damage subtracted for BAD or empty POOR (an extra input, not a missed note).
            // [3] Damage subtracted for missed-note POOR.
            //
            // Native damage entries are positive magnitudes. read_native_deltas() expands
            // these four entries into six signed deltas for our rules without changing the ABI.
            const auto result = deltas_hook.call<std::intptr_t>(p1_notes, p2_notes);
            for (unsigned player = 0; player < 2; ++player)
            {
                const auto mode = active_mode(player);
                auto& attempt = attempts[player];
                if (is_lr2(mode))
                {
                    const auto notes = double_play ? static_cast<std::int64_t>(p1_notes) + p2_notes :
                        static_cast<std::int64_t>(player == 0 ? p1_notes : p2_notes);
                    attempt.lr2.reset();
                    if (notes > 0 && notes <= std::numeric_limits<int>::max())
                        attempt.lr2 = gauge_rules::calculate_lr2(mode, static_cast<int>(notes));
                    if (!attempt.lr2)
                    {
                        report("Cannot determine LR2 chart note count; special gauges are disabled for this play.");
                        disable_play();
                        reset_hook.call<std::intptr_t>();
                        return deltas_hook.call<std::intptr_t>(p1_notes, p2_notes);
                    }
                    continue;
                }
                if (mode == type::Hazard)
                {
                    calculate_native_deltas(player, player == 0 ? p1_notes : p2_notes, false);
                    continue;
                }
                if (mode == type::Dan)
                {
                    calculate_native_deltas(player, player == 0 ? p1_notes : p2_notes, true);
                    continue;
                }
                if (mode != type::Erosion)
                    continue;
                const auto tick_ms = reinterpret_cast<float (*)()>(bm2dx::addr->GAUGE_TICK_MS)();
                const auto notes = double_play ? static_cast<double>(p1_notes) + p2_notes :
                    static_cast<double>(player == 0 ? p1_notes : p2_notes);
                auto& erosion = attempt.erosion;
                const auto calculation = gauge_rules::calculate_erosion(
                    attempt.options.erosion_level, notes, erosion.duration_seconds, tick_ms, double_play);
                if (!calculation)
                {
                    report("Cannot determine erosion chart timing; special gauges are disabled for this play.");
                    disable_play();
                    reset_hook.call<std::intptr_t>();
                    return deltas_hook.call<std::intptr_t>(p1_notes, p2_notes);
                }
                erosion.drain_percent = calculation->drain_percent;
                erosion.interval_ticks = calculation->interval_ticks;
                erosion.next_drain_tick = erosion.interval_ticks;
                erosion.end_tick = calculation->end_tick;
            }
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

            const auto mode = active_mode(player);
            if (mode == type::Default || mode == type::Dan)
            {
                judgment_hook.call<void>(player, judge);
                return;
            }

            std::optional<int> delta;
            if (is_lr2(mode))
            {
                auto& attempt = attempts[player];
                const auto current = values()[player].value;
                if (current <= 0)
                    return;

                const auto next = attempt.lr2 ? gauge_rules::lr2_value(
                    *attempt.lr2, judge, (current + attempt.lr2_fraction) / 50.0) : std::nullopt;
                if (next)
                {
                    const double raw = *next * 50.0;
                    const int target = static_cast<int>(raw);
                    apply_delta(player, target - current);
                    attempt.lr2_fraction = values()[player].value == target ? raw - target : 0;
                    return;
                }
            }
            switch (mode)
            {
                case type::Hazard:
                    delta = gauge_rules::hazard_delta(judge, values()[player].value, read_native_deltas(player));
                    break;
                case type::ExDan:
                    delta = gauge_rules::ex_dan_delta(judge);
                    break;
                case type::Erosion:
                    delta = gauge_rules::erosion_delta(attempts[player].options.erosion_level, judge);
                    break;
                default:
                    break;
            }

            if (!delta)
            {
                report("Unexpected special-gauge judgment or level; native gauge update used.");
                judgment_hook.call<void>(player, judge);
                return;
            }

            apply_delta(player, *delta);
        }

        auto tick(int frame) -> std::intptr_t
        {
            const auto result = tick_hook.call<std::intptr_t>(frame);
            if (current_phase != phase::playing)
                return result;
            for (unsigned player = 0; player < 2; ++player)
            {
                auto& erosion = attempts[player].erosion;
                if (active_mode(player) != type::Erosion || erosion.interval_ticks <= 0)
                    continue;
                while (erosion.next_drain_tick <= frame && erosion.next_drain_tick <= erosion.end_tick)
                {
                    apply_delta(player, gauge_rules::erosion_drain_delta(erosion.drain_percent, values()[player].value));
                    erosion.next_drain_tick += erosion.interval_ticks;
                }
            }
            return result;
        }

        auto select_artwork(SafetyHookContext& ctx) -> void
        {
            const auto player = static_cast<unsigned>(ctx.rsi);
            if (player >= attempts.size())
            {
                gauge_render::reset();
                return;
            }
            int native_gauge = 0;
            if ((ctx.rbp == 0 || ctx.rbp == 1) && bm2dx::state)
            {
                const auto current = options();
                const auto block = bm2dx::state->play_style == static_cast<int>(bm2dx::play_style::DP) ? 2u : player;
                // Consult the live option after GSM's selection, not the chart's starting gauge.
                if (current)
                {
                    native_gauge = current->blocks[block].normal_gauge;
                }
            }
            gauge_render::begin_draw(active_options(player), native_gauge, tint_easy_enabled.load());
        }

        auto result_clear(unsigned player) -> std::intptr_t
        {
            const auto result = result_clear_hook.call<std::intptr_t>(player);

            // Preserve Full Combo; other saving restrictions still apply.
            if (result == 7)
                return result;

            if (player >= attempts.size())
                return result;

            // The attempt's type survives results/menu edits and late submissions.
            const std::lock_guard lock(mutex);
            const auto owner = double_play ? dp_player : player;
            if (attempts[owner].failed)
                return result;

            if (attempts[owner].options.mode != type::Default)
                return 0;

            // Otherwise, return the original result.
            return result;
        }
    }

    auto available() -> bool { return installed.load(); }
    auto custom_available() -> bool { return available() && !GetModuleHandleW(L"2dx-gsm.dll"); }
    auto in_play() -> bool { return current_phase == phase::playing; }

    auto report_draw_error(const std::string& message) -> void
    {
        report(message);
    }

    auto on_sprite_draw(void* sprite, const void* caller, int horizontal, int vertical, unsigned layer) -> void
    {
        if (!available())
            return;
        gauge_render::draw_sprite(sprite, caller, horizontal, vertical, layer);
    }

    auto status() -> std::string
    {
        if (GetModuleHandleW(L"2dx-gsm.dll"))
            return gsm_warning;
        const std::lock_guard lock(mutex);
        return error;
    }

    auto update_tint() -> void
    {
        tint_easy_enabled.store(tint_easy);
    }

    auto update() -> void
    {
        const std::lock_guard lock(mutex);
        update_tint();
        for (unsigned player = 0; player < 2; ++player)
        {
            auto& value = players[player];
            if (!find_definition(value.mode))
            {
                error = "Unexpected gauge type; native gauge selected.";
                log::print("[Gauge] {}", error);
                value.mode = type::Default;
            }
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
        current_phase = phase::idle;
        gauge_render::reset();
    }

    auto finish_play() -> void
    {
        if (!available() || current_phase != phase::playing)
            return;
        const std::lock_guard lock(mutex);
        for (unsigned player = 0; player < 2; ++player)
            if (!attempts[player].failed && is_dan(attempts[player].options.mode) &&
                attempts[player].options.dan_keep)
            {
                const auto value = values()[player].value;
                carried_percent[player] = value <= 0 ? 100 : std::clamp(2 * (value / 100), 2, 100);
            }
        current_phase = phase::results;
    }

    auto clear_session() -> void
    {
        enter_select();
        const std::lock_guard lock(mutex);
        carried_percent = {};
        attempts = {};
        double_play = false;
        dp_player = 0;
        current_phase = phase::idle;
    }

    auto reset() -> void
    {
        clear_session();
        players = {};
        tint_easy = false;
        update();
    }

    auto capture_chart(std::uint8_t player, std::span<const bm2dx::chart_event_t> events) -> void
    {
        if (player >= 2)
            return;

        auto& erosion = attempts[double_play ? dp_player : player].erosion;
        for (const auto& event : events)
        {
            if (event.type == bm2dx::chart_event_type::END_OF_SONG)
            {
                // Raw chart events are in milliseconds; native playback ticks are separate.
                erosion.duration_seconds = std::max(erosion.duration_seconds, event.offset / 1000.0);
                return;
            }
        }
    }

    auto install_hook() -> void
    {
        if (!bm2dx::addr->GAUGE_RESET || !bm2dx::addr->GAUGE_DELTA_SETUP ||
            !bm2dx::addr->GAUGE_JUDGMENT || !bm2dx::addr->GAUGE_TICK ||
            !bm2dx::addr->GAUGE_APPLY_DELTA || !bm2dx::addr->GAUGE_TICK_MS ||
            !bm2dx::addr->GAUGE_VALUES || !bm2dx::addr->GAUGE_SPECIAL_STATE ||
            !bm2dx::addr->GAUGE_ARTWORK || !bm2dx::addr->BM2D_RENDERER ||
            !bm2dx::addr->GAUGE_RESULT_CLEAR || !bm2dx::addr->GAUGE_DELTA_MAGNITUDE)
        {
            report("Special gauges are unavailable for this game build.");
            return;
        }
        reset_hook = safetyhook::create_inline(bm2dx::addr->GAUGE_RESET, reset_native);
        deltas_hook = safetyhook::create_inline(bm2dx::addr->GAUGE_DELTA_SETUP, calculate_deltas);
        judgment_hook = safetyhook::create_inline(bm2dx::addr->GAUGE_JUDGMENT, judgment);
        tick_hook = safetyhook::create_inline(bm2dx::addr->GAUGE_TICK, tick);
        artwork_hook = safetyhook::create_mid(bm2dx::addr->GAUGE_ARTWORK, select_artwork);
        result_clear_hook = safetyhook::create_inline(bm2dx::addr->GAUGE_RESULT_CLEAR, result_clear);
        if (!reset_hook || !deltas_hook || !judgment_hook || !tick_hook || !artwork_hook ||
            !result_clear_hook)
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
