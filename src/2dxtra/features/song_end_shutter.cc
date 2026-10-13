#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <mutex>
#include <optional>
#include "song_end_shutter.h"
#include "../game.h"
#include "../log.h"
#include "../hooks/judge_timing_hook.h"

namespace iidxtra::song_end_shutter
{
    bool enabled = false;

    namespace
    {
        using clock = std::chrono::steady_clock;
        constexpr auto judgment_pause = std::chrono::milliseconds {300};
        constexpr auto descent_duration = std::chrono::milliseconds {600};
        constexpr int cover_height = 723;
        constexpr unsigned native_cover_layer = 108;
        constexpr unsigned shutter_layer = 106; // In front of covers (108) and their readouts (107).
        constexpr std::size_t cover_controllers_offset = 0xFE0;
        constexpr std::size_t playback_tick_offset = 0x5C;

        struct cover_control
        {
            int player;
            std::byte unused_04[4];
            void* artwork;
            std::byte unused_10[8];
            void* numbers;
            std::byte unused_20[8];
            bool visible;
            std::byte unused_29[15];
        };
        static_assert(sizeof(cover_control) == 0x38);
        static_assert(offsetof(cover_control, visible) == 0x28);

        struct side_state
        {
            bool active = false;
            std::array<std::optional<std::int64_t>, 2> last_ticks;
            std::optional<std::array<float, 2>> late_windows;
            std::optional<int> start_y;
        };
        enum class phase { idle, waiting, descending, closed };
        struct attempt_state
        {
            phase state = phase::idle;
            bool double_play = false;
            std::array<side_state, 2> sides;
            std::optional<clock::time_point> judgment_finished;
            const char* last_error = nullptr;
        };
        attempt_state attempt;
        std::atomic_bool requested = false;
        bool ready = false;
        std::mutex mutex;
        std::array<void*, 2> raised_artwork {};

        auto set_artwork_layer(void* artwork, unsigned layer) -> void
        {
            // Native cover wrappers hold either a static sprite or an animated layer.
            const auto* objects = static_cast<void* const*>(artwork);
            for (unsigned i = 0; i < 2; ++i)
                if (auto* object = objects[i])
                {
                    const auto vtable = *static_cast<void***>(object);
                    reinterpret_cast<void(*)(void*, unsigned)>(vtable[30])(object, layer);
                }
        }

        auto report(const char* message) -> void
        {
            if (attempt.last_error != message)
                log::print("[Song End Shutter] {}", message);
            attempt.last_error = message;
        }

        auto ready_to_descend(clock::time_point now) -> bool
        {
            const auto& a = *bm2dx::addr;
            const auto tick = *reinterpret_cast<const int*>(a.HI_SPEED_STATE + playback_tick_offset);
            const auto tick_ms = reinterpret_cast<float(*)()>(a.GAUGE_TICK_MS)();
            if (!std::isfinite(tick_ms) || tick_ms <= 0)
            {
                report("Playback timing is unavailable; cover animation skipped.");
                attempt.judgment_finished.reset();
                return false;
            }
            double deadline = -1;
            for (unsigned player = 0; player < attempt.sides.size(); ++player)
                for (unsigned kind = 0; kind < 2; ++kind)
                    if (const auto last = attempt.sides[player].last_ticks[kind])
                    {
                        const auto& windows = attempt.sides[player].late_windows;
                        if (!windows)
                        {
                            report("Judgment windows are unavailable; shutter animation deferred.");
                            attempt.judgment_finished.reset();
                            return false;
                        }
                        const auto late = (*windows)[kind];
                        if (!std::isfinite(late) || late < 0)
                        {
                            report("Invalid late-judgment window; cover animation skipped.");
                            attempt.judgment_finished.reset();
                            return false;
                        }
                        deadline = std::max(deadline, *last * static_cast<double>(tick_ms) + late);
                    }
            if (deadline < 0)
                return false;
            if (tick * static_cast<double>(tick_ms) <= deadline)
            {
                attempt.judgment_finished.reset();
                return false;
            }
            if (!attempt.judgment_finished)
                attempt.judgment_finished = now;
            return now - *attempt.judgment_finished >= judgment_pause;
        }

        auto draw_side(cover_control& control, unsigned player, double remaining) -> void
        {
            auto& side = attempt.sides[player];
            if (control.player != static_cast<int>(player) || !control.artwork)
            {
                report("Lane-cover artwork is unavailable; cover animation skipped.");
                return;
            }
            const auto& a = *bm2dx::addr;
            if (!side.start_y)
            {
                int amount = 0;
                if (control.visible)
                {
                    auto* options = reinterpret_cast<void*(*)()>(a.GET_PLAYER_OPTIONS)();
                    if (!options)
                    {
                        report("Player options are unavailable; cover animation skipped.");
                        return;
                    }
                    amount = reinterpret_cast<int(*)(void*, int, int)>(a.GET_SUDDEN_POSITION)(
                        options, player, attempt.double_play);
                }
                side.start_y = std::clamp(amount, 0, cover_height) - cover_height;
            }
            const auto* positions = reinterpret_cast<const int*>(a.PLAY_COVER_POS);
            const int y = static_cast<int>(std::lround(*side.start_y * remaining));
            set_artwork_layer(control.artwork, shutter_layer);
            raised_artwork[player] = control.artwork;
            reinterpret_cast<void(*)(void*, int, int)>(a.COVER_POSITION_FN)(
                control.artwork, positions[2 * int(attempt.double_play) + player], y);
            if (control.numbers)
            {
                const auto vtable = *static_cast<void***>(control.numbers);
                reinterpret_cast<void(*)(void*, bool)>(vtable[5])(control.numbers, false);
            }
        }

        auto draw_at(bm2dx::play_session_t* session, clock::time_point now) -> void
        {
            const std::lock_guard lock(mutex);
            if (attempt.state == phase::idle || !requested || !session || !bm2dx::state ||
                bm2dx::state->game_type == 9)
                return;
            const auto dead = bm2dx::dead_state;
            std::array<bool, 2> active {};
            for (unsigned player = 0; player < active.size(); ++player)
            {
                const auto owner = attempt.double_play ? (bm2dx::state->p1_active ? 0u : 1u) : player;
                const bool failed = dead && (owner == 0 ? dead->p1 : dead->p2);
                active[player] = attempt.sides[player].active && !failed;
            }
            if (!active[0] && !active[1])
            {
                attempt = {};
                return;
            }
            if (attempt.state == phase::waiting)
            {
                if (!ready_to_descend(now))
                    return;
                attempt.state = phase::descending;
            }
            double remaining = 0;
            if (attempt.state == phase::descending)
            {
                const auto elapsed = now - *attempt.judgment_finished - judgment_pause;
                const double progress = std::clamp(
                    std::chrono::duration<double, std::milli>(elapsed).count() / descent_duration.count(), 0.0, 1.0);
                // Cubic ease-out: move promptly, then continuously slow to a stop.
                remaining = (1.0 - progress) * (1.0 - progress) * (1.0 - progress);
                if (progress == 1.0)
                    attempt.state = phase::closed;
            }
            auto* controls = reinterpret_cast<cover_control*>(
                reinterpret_cast<std::byte*>(session) + cover_controllers_offset);
            for (unsigned player = 0; player < active.size(); ++player)
                if (active[player])
                    draw_side(controls[player], player, remaining);
        }
    }

    auto initialize(bool draw_hook_available) -> void
    {
        const auto& a = *bm2dx::addr;
        ready = draw_hook_available && judge_timing_hook::available() && a.PLAY_COVER_POS && a.COVER_POSITION_FN &&
            a.GET_SUDDEN_POSITION && a.GET_PLAYER_OPTIONS &&
            a.GAUGE_TICK_MS && a.HI_SPEED_STATE;
        if (!ready)
            log::print("[Song End Shutter] Required draw hook or game addresses are unavailable.");
    }

    auto available() -> bool { return ready; }
    auto update() -> void { requested = enabled && ready; }

    auto in_play() -> bool
    {
        const std::lock_guard lock(mutex);
        return attempt.state != phase::idle;
    }

    auto end_play() -> void
    {
        const std::lock_guard lock(mutex);
        attempt = {};
    }

    auto reset() -> void
    {
        enabled = false;
        update();
        end_play();
    }

    auto capture_timing(unsigned player, const bm2dx::player_timing_t& timing) -> void
    {
        const std::lock_guard lock(mutex);
        if (player >= attempt.sides.size())
        {
            report("Invalid timing player; shutter update skipped.");
            return;
        }
        // Only accept this attempt's post-override timing events; retries clear both sides.
        if (attempt.state == phase::waiting)
            attempt.sides[player].late_windows = {timing.keys.late_bad, timing.scratch.late_bad};
    }

    auto begin_play(const bm2dx::play_field_t* field) -> void
    {
        const std::lock_guard lock(mutex);
        attempt = {};
        if (!ready || !field || !bm2dx::state || bm2dx::state->game_type == 9)
            return;
        attempt.double_play = bm2dx::state->play_style != 0;
        for (unsigned player = 0; player < attempt.sides.size(); ++player)
        {
            auto& side = attempt.sides[player];
            side.active = attempt.double_play ? bm2dx::state->p1_active || bm2dx::state->p2_active :
                (player == 0 ? bm2dx::state->p1_active : bm2dx::state->p2_active);
            if (!side.active)
                continue;
            const int count = field->note_counts[player];
            if (count < 0 || count > bm2dx::MAX_PLAY_NOTES)
            {
                report("Invalid play-field note count; cover animation skipped.");
                attempt = {};
                return;
            }
            for (int index = 0; index < count; ++index)
            {
                const auto& note = field->notes[player].notes[index];
                if (note.type != 0 && note.type != 5 && note.type != 6)
                    continue;
                if (note.lane1 < 0 || note.lane1 > bm2dx::SCRATCH_COLUMN || note.tick < 0)
                {
                    report("Invalid playable note; cover animation skipped.");
                    attempt = {};
                    return;
                }
                const auto end = static_cast<std::int64_t>(note.tick) + std::max(note.cn_duration, 0);
                auto& last = side.last_ticks[note.lane1 == bm2dx::SCRATCH_COLUMN ? 1 : 0];
                last = std::max(last.value_or(end), end);
            }
        }
        attempt.state = phase::waiting;
    }

    auto draw(bm2dx::play_session_t* session) -> void
    {
        draw_at(session, clock::now());
    }

    auto begin_frame(bm2dx::play_session_t* session) -> void
    {
        const std::lock_guard lock(mutex);
        if (session)
        {
            const auto* controls = reinterpret_cast<const cover_control*>(
                reinterpret_cast<const std::byte*>(session) + cover_controllers_offset);
            for (unsigned player = 0; player < raised_artwork.size(); ++player)
                // Never dereference a saved wrapper after native cleanup/replacement.
                if (raised_artwork[player] && raised_artwork[player] == controls[player].artwork)
                    set_artwork_layer(controls[player].artwork, native_cover_layer);
        }
        raised_artwork = {};
    }
}
