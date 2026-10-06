#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <deque>
#include <mutex>
#include <optional>
#include "live_timing.h"
#include "key_release.h"
#include "../gui/timing_colors.h"
#include "../hooks/fast_slow_hook.h"
#include "../judgment.h"
#include "../log.h"

namespace iidxtra::live_timing
{
    mode display_mode = mode::Off;
    int y_position = default_y;

    namespace
    {
        // State and shared helpers

        using clock = std::chrono::steady_clock;

        // Signed ticks are stored at index (tick + tick_limit).
        constexpr int tick_limit = 64;
        constexpr int tick_count = tick_limit * 2 + 1;
        constexpr float fade_seconds = 5.0f;

        struct hit
        {
            clock::time_point time;
            int color;
        };

        struct timing_sample
        {
            clock::time_point time;
            float milliseconds;
            bool scratch;
        };

        struct bar_state
        {
            std::array<std::optional<hit>, tick_count> hits;
            std::optional<float> average_ms;
        };

        struct player_state
        {
            bar_state combined;
            bar_state notes;
            bar_state scratch;
            std::deque<timing_sample> recent_samples;
        };

        std::mutex mutex;
        std::array<player_state, 2> players;
        mode current_mode = mode::Off;
        bool playing = false;
        bool double_play = false;
        std::string error;

        auto report(const char* message) -> void
        {
            if (error == message)
                return;

            error = message;
            log::print("[Live Timing] {}", message);
        }

        auto clear_hits() -> void
        {
            for (auto& player : players)
            {
                player.combined = {};
                player.notes = {};
                player.scratch = {};
                player.recent_samples.clear();
            }
        }

        auto intensity(const hit& value, clock::time_point now) -> float
        {
            const auto age = std::chrono::duration<float>(now - value.time).count();
            return std::clamp(1.0f - age / fade_seconds, 0.0f, 1.0f);
        }

        auto color_for_code(int code) -> int
        {
            switch (code)
            {
                case bm2dx::judge_display_code::pgreat:
                    return 0;

                case bm2dx::judge_display_code::early_great:
                case bm2dx::judge_display_code::late_great:
                    return 1;

                case bm2dx::judge_display_code::early_good:
                case bm2dx::judge_display_code::late_good:
                    return 2;

                case bm2dx::judge_display_code::early_bad:
                case bm2dx::judge_display_code::late_bad:
                    return 3;

                default:
                    return -1;
            }
        }

        auto update_average(player_state& player, clock::time_point now) -> void
        {
            const auto cutoff = now - std::chrono::seconds(5);
            while (!player.recent_samples.empty() && player.recent_samples.front().time <= cutoff)
                player.recent_samples.pop_front();

            // An empty window retains the last arrow position, until the next hit or reset.
            if (player.recent_samples.empty())
                return;

            std::array<double, 2> totals {};
            std::array<std::size_t, 2> counts {};
            for (const auto& sample : player.recent_samples)
            {
                const auto channel = sample.scratch ? 1 : 0;
                totals[channel] += sample.milliseconds;
                ++counts[channel];
            }

            player.combined.average_ms = static_cast<float>(
                (totals[0] + totals[1]) / player.recent_samples.size());
            if (counts[0] != 0)
                player.notes.average_ms = static_cast<float>(totals[0] / counts[0]);
            if (counts[1] != 0)
                player.scratch.average_ms = static_cast<float>(totals[1] / counts[1]);
        }

        auto color_for_note(const bm2dx::timing_t& windows, float milliseconds) -> int
        {
            // Reclassify scratch timing against note windows for the combined bar.
            if (milliseconds <= windows.early_poor || milliseconds > windows.late_bad)
                return 4;
            if (milliseconds <= windows.early_bad)
                return 3;
            if (milliseconds <= windows.early_good)
                return 2;
            if (milliseconds <= windows.early_great)
                return 1;
            if (milliseconds <= windows.late_pgreat)
                return 0;
            if (milliseconds <= windows.late_great)
                return 1;
            if (milliseconds <= windows.late_good)
                return 2;
            return 3;
        }

        auto record_at(int player, int tick, float milliseconds, int code, clock::time_point now,
                       bool scratch, const bm2dx::timing_t& note_windows) -> void
        {
            const auto color = color_for_code(code);
            if (current_mode == mode::Off || !playing || color < 0)
                return;

            if (player < 0 || player >= 2 || tick < -tick_limit || tick > tick_limit ||
                !std::isfinite(milliseconds))
            {
                report("Unsupported player or timing tick; sample ignored.");
                return;
            }
            // DP combines both input sides in the first bar.
            const int bar = double_play ? 0 : player;
            const int index = tick + tick_limit;

            // Replacing the timestamp restarts this tick's five-second fade.
            auto& state = players[bar];
            auto& channel = scratch ? state.scratch : state.notes;
            channel.hits[index] = hit {now, color};
            const auto combined_color = scratch ? color_for_note(note_windows, milliseconds) : color;
            state.combined.hits[index] = hit {now, combined_color};

            // Unlike the tick highlights, the average counts every press, including repeats.
            state.recent_samples.push_back({now, milliseconds, scratch});
            update_average(state, now);
        }
    }

    // Settings and status

    auto max_y_position() -> int
    {
        return display_mode == mode::Split ? max_y - bar_spacing : max_y;
    }

    auto available() -> bool
    {
        return fast_slow_hook::available() && bm2dx::addr &&
            bm2dx::addr->PLAY_NOTE_POS && bm2dx::addr->GAUGE_TICK_MS;
    }

    auto status() -> std::string
    {
        const std::lock_guard lock(mutex);

        if (!available())
            return "Live timing indicator is unavailable for this game build.";

        return error;
    }

    auto update() -> void
    {
        const std::lock_guard lock(mutex);
        y_position = std::clamp(y_position, 0, max_y_position());

        // Moving the bar preserves hits; selecting a different mode starts fresh.
        if (current_mode != display_mode)
            clear_hits();

        current_mode = display_mode;
    }

    auto reset() -> void
    {
        display_mode = mode::Off;
        y_position = default_y;
        update();

        const std::lock_guard lock(mutex);
        playing = false;
        players = {};
        error.clear();
    }

    // Play lifecycle

    auto begin_play(bool is_dp) -> void
    {
        key_release::set_in_song(bm2dx::state && bm2dx::state->game_type != 9);
        const std::lock_guard lock(mutex);
        clear_hits();
        double_play = is_dp;
        playing = true;
        error.clear();
    }

    auto end_play() -> void
    {
        key_release::set_in_song(false);
        const std::lock_guard lock(mutex);
        playing = false;
        clear_hits();
    }

    // Timing capture

    auto in_play() -> bool
    {
        const std::lock_guard lock(mutex);
        return playing;
    }

    auto is_recording() -> bool
    {
        const std::lock_guard lock(mutex);
        return current_mode != mode::Off && playing;
    }

    auto record_note(int player, bool scratch, int tick, float milliseconds, int display_code,
                     const bm2dx::timing_t& note_windows) -> void
    {
        const std::lock_guard lock(mutex);
        record_at(player, tick, milliseconds, display_code, clock::now(), scratch, note_windows);
    }

    // Bar layout and drawing

    namespace
    {
        struct bar_layout
        {
            int first_tick;
            int last_tick;
            float left;
            float box_width;
            float top;
            float bottom;
            ImVec2 display_size;
            float clip_left;
            float clip_right;
        };

        auto make_bar_layout(int player, float tick_ms,
                             const std::array<std::int32_t, 32>& lanes,
                             ImVec2 display_size) -> std::optional<bar_layout>
        {
            // Tick 0 ends at display zero; tick 1 starts there. Round the
            // +/-200 ms range outward to whole ticks so edge boxes stay full-width.
            const auto ticks_per_half = std::ceil(range_ms / tick_ms);
            if (!std::isfinite(ticks_per_half) || ticks_per_half < 1 || ticks_per_half > tick_limit)
            {
                report("Unsupported timing tick duration; indicator hidden.");
                return std::nullopt;
            }

            const int first = 1 - static_cast<int>(ticks_per_half);
            const int last = static_cast<int>(ticks_per_half);

            // Fit the bar to the key lanes, excluding the outer scratches.
            int left_group = player * 8;
            int right_group = left_group;
            if (double_play)
            {
                left_group = 16;
                right_group = 24;
            }

            // Positions are left edges. Key 7 has the same width as key 1,
            // whose width is the distance from its left edge to key 2.
            const int first_lane = left_group;
            const int last_lane = right_group + 6;
            const int key_width = lanes[right_group + 1] - lanes[right_group];
            float left = static_cast<float>(lanes[first_lane]);
            float right = static_cast<float>(lanes[last_lane] + key_width);
            const float center = (left + right) * 0.5f;

            // DP keeps its combined center, but uses the same seven-key width as SP.
            if (double_play)
            {
                const int sp_key_width = lanes[1] - lanes[0];
                const float sp_width = static_cast<float>(lanes[6] + sp_key_width - lanes[0]);
                left = center - sp_width * 0.5f;
                right = center + sp_width * 0.5f;
            }

            const float box_width = (right - left) / (last - first + 1);
            const float top = y_position * display_size.y / 1080.0f;
            const float bottom = top + 9.6f * display_size.y / 1080.0f;

            return bar_layout {first, last, left, box_width, top, bottom, display_size, left, right};
        }

        auto draw_tick_boxes(ImDrawList* draw, const bar_layout& layout,
                             const bar_state& bar, clock::time_point now) -> void
        {
            // Draw only recent hits. Adjacent boxes share an edge, with no
            // border or gap; opacity fades from 70% to fully transparent.
            for (int tick = layout.first_tick; tick <= layout.last_tick; ++tick)
            {
                // Padding is layout-only and may extend beyond the stored tick range.
                if (tick < -tick_limit || tick > tick_limit)
                    continue;

                const auto& sample = bar.hits[tick + tick_limit];
                if (!sample)
                    continue;

                const float alpha = 0.7f * intensity(*sample, now);
                if (alpha <= 0)
                    continue;

                const int box = tick - layout.first_tick;
                const float left = (layout.left + box * layout.box_width) *
                    layout.display_size.x / 1920.0f;
                const float right = (layout.left + (box + 1) * layout.box_width) *
                    layout.display_size.x / 1920.0f;

                // The combined bar stores note-based colors; split bars store actual judgments.
                const auto rgb = gui::timing_colors[sample->color] & ~IM_COL32_A_MASK;
                const auto opacity = static_cast<ImU32>(alpha * 255) << IM_COL32_A_SHIFT;
                draw->AddRectFilled({left, layout.top}, {right, layout.bottom}, rgb | opacity);
            }
        }

        auto draw_center_marker(ImDrawList* draw, const bar_layout& layout, bool scratch = false) -> void
        {
            if (layout.first_tick > 0 || layout.last_tick < 1)
                return;

            // The persistent center marker sits between ticks 0 and 1,
            // extending six pixels above and below the bar at 1080p.
            const int center_boundary = 1 - layout.first_tick;
            const float center = (layout.left + center_boundary * layout.box_width) * layout.display_size.x / 1920.0f;
            const float half_width = 0.5f * layout.display_size.x / 1920.0f;
            const float marker_top = layout.top - 6.0f * layout.display_size.y / 1080.0f;
            const float marker_bottom = layout.bottom + 6.0f * layout.display_size.y / 1080.0f;

            draw->AddRectFilled(
                {center - half_width, marker_top},
                {center + half_width, marker_bottom},
                scratch ? IM_COL32(255, 0, 0, 204) : IM_COL32(255, 255, 255, 204));
        }

        auto draw_average_arrow(ImDrawList* draw, const bar_layout& layout,
                                const bar_state& bar, float tick_ms) -> void
        {
            if (!bar.average_ms)
                return;

            // Display zero is halfway between native ticks 0 and 1, like FAST/SLOW milliseconds.
            const float centered_ms = *bar.average_ms - tick_ms * 0.5f;
            const float center_boundary = 1.0f - layout.first_tick;
            const float average_boundary = center_boundary + centered_ms / tick_ms;
            const float position = layout.left + average_boundary * layout.box_width;
            const float x = std::clamp(position, layout.clip_left, layout.clip_right) *
                layout.display_size.x / 1920.0f;

            const float half_width = 4.0f * layout.display_size.x / 1920.0f;
            const float tip_y = layout.bottom + 4.0f * layout.display_size.y / 1080.0f;
            const float base_y = layout.bottom + 10.0f * layout.display_size.y / 1080.0f;
            draw->AddTriangleFilled({x, tip_y}, {x + half_width, base_y},
                {x - half_width, base_y}, IM_COL32(255, 255, 255, 178));
        }
    }

    auto render() -> void
    {
        // The native in_gameplay flag can be false during DP songs.
        // Use our play-field/result lifecycle below, while still excluding attract demos.
        if (display_mode == mode::Off || !available() || !bm2dx::state || bm2dx::state->game_type == 9)
            return;

        const auto tick_ms = reinterpret_cast<float (*)()>(bm2dx::addr->GAUGE_TICK_MS)();
        const auto now = clock::now();
        const std::lock_guard lock(mutex);

        if (current_mode == mode::Off || !playing)
            return;

        if (!std::isfinite(tick_ms) || tick_ms <= 0)
        {
            report("Cannot determine native timing tick duration.");
            return;
        }

        // Read the live layout so SP scratch-side flipping also moves the bar.
        // Table order: SP P1, SP P2, DP left, DP right; eight lanes per group.
        std::array<std::int32_t, 32> lanes;
        std::memcpy(lanes.data(), bm2dx::addr->PLAY_NOTE_POS, sizeof(lanes));

        const auto display_size = ImGui::GetIO().DisplaySize;
        auto* draw = ImGui::GetBackgroundDrawList();

        for (int player = 0; player < 2; ++player)
        {
            // SP draws one bar per active player. DP combines both sides in player 0's bar.
            if (double_play)
            {
                if (player != 0)
                    continue;

                if (!bm2dx::state->p1_active && !bm2dx::state->p2_active)
                    continue;

            }
            else
            {
                const bool active = player == 0 ?
                    bm2dx::state->p1_active != 0 : bm2dx::state->p2_active != 0;
                if (!active)
                    continue;
            }

            const auto layout = make_bar_layout(player, tick_ms, lanes, display_size);
            if (!layout)
                continue;

            update_average(players[player], now);
            if (current_mode == mode::Combined)
            {
                draw_tick_boxes(draw, *layout, players[player].combined, now);
                draw_center_marker(draw, *layout);
                draw_average_arrow(draw, *layout, players[player].combined, tick_ms);
                continue;
            }

            draw_tick_boxes(draw, *layout, players[player].notes, now);
            draw_center_marker(draw, *layout);
            draw_average_arrow(draw, *layout, players[player].notes, tick_ms);

            auto scratch_layout = *layout;
            const float offset = bar_spacing * display_size.y / 1080.0f;
            scratch_layout.top += offset;
            scratch_layout.bottom += offset;
            draw_tick_boxes(draw, scratch_layout, players[player].scratch, now);
            draw_center_marker(draw, scratch_layout, true);
            draw_average_arrow(draw, scratch_layout, players[player].scratch, tick_ms);
        }
    }
}
