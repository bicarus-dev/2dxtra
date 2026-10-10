#include <array>
#include <algorithm>
#include <limits>
#include <optional>
#include <string>
#include <picosha2.h>

#include "urafumen_experimental.h"

namespace
{
    using urafumen_experimental::event;
    using urafumen_experimental::event_type;
    using urafumen_experimental::column;
    using enum event_type;
    using enum column;

    auto constexpr NO_VALUE = std::numeric_limits<std::uint16_t>::max();
    auto constexpr BYTE_COUNTER_MASK = std::numeric_limits<std::uint8_t>::max();
    auto constexpr ORIGINAL_NOTE_MARKER = std::uint8_t { 0xAA };
    auto constexpr BGM_CANDIDATE_MARKER = std::uint8_t { 0xCC };
    auto constexpr UNKNOWN_END_OFFSET = std::numeric_limits<std::int32_t>::max();

    auto constexpr EXTENDED_NOTE_P1 = static_cast<event_type>(100);
    auto constexpr EXTENDED_NOTE_P2 = static_cast<event_type>(101);
    auto constexpr SCRATCH_LANE = urafumen_experimental::SCRATCH_COL;
    auto constexpr LINKED_SCRATCH_LANE = 107;

    auto constexpr GAP_DECAY_THRESHOLD = 350;
    auto constexpr GAP_STEP = 10;
    auto constexpr SAME_SAMPLE_GAP = 80;
    auto constexpr DP_SPACING_MULTIPLIER = 4;

    auto constexpr PLAYABLE_LANE_COUNT = 7;
    auto constexpr COLUMN_COUNT = 8;
    auto constexpr SAMPLE_SLOT_COUNT = 9;
    auto constexpr DP_SIDE_COUNT = 2;
    auto constexpr DP_KEY_COUNT = DP_SIDE_COUNT * PLAYABLE_LANE_COUNT;

    auto constexpr KIRAKU_SPACING_MS = 250;
    auto constexpr KICHIKU_SPACING_MS = 200;
    auto constexpr KIRAKU_DENSITY_CAP = 50;
    auto constexpr KICHIKU_DENSITY_CAP = 100;
    auto constexpr KIRAKU_MEASURE_INTERVAL = 8;
    auto constexpr KICHIKU_MEASURE_INTERVAL = 4;
    auto constexpr KIRAKU_CHORD_CAP = 3;
    auto constexpr KICHIKU_CHORD_CAP = 5;
    auto constexpr KIRAKU_SCRATCH_MARGIN_MS = 600;
    auto constexpr KICHIKU_SCRATCH_MARGIN_MS = 400;
    auto constexpr HAND_ACTIVITY_WINDOW_MS = 200;
    auto constexpr OCCUPIED_KEY_WEIGHT = 4;
    auto constexpr NEARBY_HEAD_WEIGHT = 1;
    auto constexpr MAX_HOLD_DURATION_MS = std::numeric_limits<std::uint16_t>::max();

    auto constexpr HIGH_KEY_FIRST_PRIORITY = std::array { 6, 4, 2, 0, 5, 1, 3 };
    auto constexpr LOW_KEY_FIRST_PRIORITY = std::array { 0, 2, 4, 6, 1, 5, 3 };

    struct generation_settings
    {
        int threshold;
        int density_cap;
        int measure_interval;
        int chord_cap;
        int scratch_margin;

        explicit generation_settings(const bool kichiku):
            threshold { kichiku ? KICHIKU_SPACING_MS: KIRAKU_SPACING_MS },
            density_cap { kichiku ? KICHIKU_DENSITY_CAP: KIRAKU_DENSITY_CAP },
            measure_interval { kichiku ? KICHIKU_MEASURE_INTERVAL: KIRAKU_MEASURE_INTERVAL },
            chord_cap { kichiku ? KICHIKU_CHORD_CAP: KIRAKU_CHORD_CAP },
            scratch_margin { kichiku ? KICHIKU_SCRATCH_MARGIN_MS: KIRAKU_SCRATCH_MARGIN_MS } {}
    };

    struct placement_candidate
    {
        std::int32_t offset = 0;
        std::uint16_t sample_value = 0;
        std::uint16_t freeze_length = 0;
        std::uint8_t lane_plus_one = 0;
        std::uint8_t marker = 0;
        std::uint8_t scratch_sides = 0;

        auto scratch_blocks(const int lane) const -> bool
            { return (scratch_sides & (1u << (lane / PLAYABLE_LANE_COUNT))) != 0; }
    };

    auto mark_dp_scratch_exclusions(std::vector<placement_candidate>& candidates,
        std::span<const event> chart, const int margin) -> void
    {
        struct interval
        {
            std::int64_t begin;
            std::int64_t end;
        };
        std::array<std::vector<interval>, DP_SIDE_COUNT> intervals;
        for (const auto& note : chart)
        {
            if (note.type == END_OF_SONG)
                break;
            if ((note.type != NOTE_P1 && note.type != NOTE_P2 &&
                 note.type != EXTENDED_NOTE_P1 && note.type != EXTENDED_NOTE_P2) ||
                (note.parameter != SCRATCH_LANE && note.parameter != LINKED_SCRATCH_LANE))
                continue;
            const auto start = static_cast<std::int64_t>(note.offset);
            const auto side = note.type == NOTE_P2 || note.type == EXTENDED_NOTE_P2 ? 1 : 0;
            // CN/BSS/MSS duration is unsigned in the chart record.
            intervals[side].push_back({start - margin,
                start + static_cast<std::uint16_t>(note.value) + margin});
        }

        for (unsigned side = 0; side < intervals.size(); ++side)
        {
            auto& ranges = intervals[side];
            std::sort(ranges.begin(), ranges.end(),
                [](const interval& a, const interval& b) { return a.begin < b.begin; });
            std::size_t merged = 0;
            for (const auto range : ranges)
            {
                if (merged != 0 && range.begin <= ranges[merged - 1].end)
                    ranges[merged - 1].end = std::max(ranges[merged - 1].end, range.end);
                else
                    ranges[merged++] = range;
            }
            ranges.resize(merged);

            std::size_t next = 0;
            for (auto& candidate : candidates)
            {
                if (candidate.lane_plus_one != 0)
                    continue;
                while (next < ranges.size() && ranges[next].end < candidate.offset)
                    ++next;
                if (next < ranges.size() && ranges[next].begin <= candidate.offset)
                    candidate.scratch_sides |= static_cast<std::uint8_t>(1u << side);
            }
        }
    }

    struct weighted_sample
    {
        std::uint16_t value = 0;
        int occurrences = 0;
    };

    struct generation_state
    {
        int next_candidate = 0;
        int candidate_count = 0;
        int priority_toggle = 0;
    };

    auto next_note_offset_on_lane(const std::vector<placement_candidate>& candidates,
        const int index, const int lane) -> std::int32_t
    {
        auto const count = static_cast<int>(candidates.size());
        auto const wanted = static_cast<std::uint8_t>(lane + 1);

        for (auto scan = index + 1; scan < count; ++scan)
            if (candidates[scan].lane_plus_one == wanted)
                return candidates[scan].offset;

        return 0;
    }

    struct hand_activity
    {
        std::array<int, DP_SIDE_COUNT> occupied {};
        std::array<int, DP_SIDE_COUNT> nearby {};

        auto workload(const int side) const -> int
        {
            return occupied[side] * OCCUPIED_KEY_WEIGHT + nearby[side] * NEARBY_HEAD_WEIGHT;
        }
    };

    auto hand_activity_at_tick(const std::vector<placement_candidate>& candidates,
        const int index) -> hand_activity
    {
        hand_activity activity;
        std::array<bool, DP_KEY_COUNT> occupied {};
        const auto offset = static_cast<std::int64_t>(candidates[index].offset);
        const auto begin = std::lower_bound(candidates.begin(), candidates.end(), offset - MAX_HOLD_DURATION_MS,
            [](const placement_candidate& note, std::int64_t time) { return note.offset < time; });
        for (auto it = begin; it != candidates.end() && it->offset <= offset + HAND_ACTIVITY_WINDOW_MS; ++it)
        {
            if (it->lane_plus_one == 0)
                continue;
            const int lane = it->lane_plus_one - 1;
            const int side = lane / PLAYABLE_LANE_COUNT;
            // A tail still occupies its key at the release timestamp. Count each key once.
            if (it->offset <= offset && offset <= static_cast<std::int64_t>(it->offset) + it->freeze_length)
            {
                if (!occupied[lane])
                {
                    occupied[lane] = true;
                    ++activity.occupied[side];
                }
            }
            else if (it->offset >= offset - HAND_ACTIVITY_WINDOW_MS)
                ++activity.nearby[side];
        }
        return activity;
    }

    auto notes_per_hand_at_tick(const std::vector<placement_candidate>& candidates,
        const int index) -> std::array<int, DP_SIDE_COUNT>
    {
        return hand_activity_at_tick(candidates, index).occupied;
    }

    struct hand_workload
    {
        std::int64_t total = 0;
        int occurrences = 0;
    };

    auto prefer_less_busy_hand(std::array<int, DP_KEY_COUNT + 1>& lane_accepts,
        const std::array<hand_workload, DP_SIDE_COUNT>& workloads) -> void
    {
        if (workloads[0].occurrences == 0 || workloads[1].occurrences == 0)
            return;
        // Compare averages, so a hand is not penalized merely for accepting more occurrences.
        const auto left = workloads[0].total * workloads[1].occurrences;
        const auto right = workloads[1].total * workloads[0].occurrences;
        if (left == right)
            return;
        const int preferred = left < right ? 0 : 1;
        const auto first = lane_accepts.begin() + preferred * PLAYABLE_LANE_COUNT;
        if (std::none_of(first, first + PLAYABLE_LANE_COUNT, [](int accepts) { return accepts > 0; }))
            return;
        const auto other = lane_accepts.begin() + (1 - preferred) * PLAYABLE_LANE_COUNT;
        std::fill(other, other + PLAYABLE_LANE_COUNT, 0);
    }

    template<std::size_t KeyCount>
    struct lane_frame
    {
        std::array<std::int32_t, KeyCount + 1> samples {};
        std::array<std::int32_t, KeyCount + 1> free_from {};

        lane_frame()
        {
            samples.fill(NO_VALUE);
        }

        auto lane_sample(const int lane) const -> std::int32_t
            { return samples[lane]; }
        auto lane_free_from(const int lane) const -> std::int32_t
            { return free_from[lane]; }

        auto set_lane_sample(const int lane, const std::int32_t value) -> void
            { samples[lane] = value; }
        auto set_lane_free_from(const int lane, const std::int32_t value) -> void
            { free_from[lane] = value; }

        auto record_real_note(const placement_candidate& rec) -> void
        {
            auto const lane = rec.lane_plus_one - 1;
            set_lane_free_from(lane, rec.offset + rec.freeze_length);
            set_lane_sample(lane, rec.sample_value);
        }
    };

    auto note_type_for_player(const int player) -> event_type
        { return static_cast<event_type>(player); }

    auto sample_type_for_player(const int player) -> event_type
        { return static_cast<event_type>(static_cast<int>(SAMPLE_P1) + player); }

    auto density_limiter(const int gap, std::uint8_t& counter,
        const generation_settings& settings) -> bool
    {
        if (gap >= GAP_DECAY_THRESHOLD)
        {
            auto const decay = (gap - GAP_DECAY_THRESHOLD) / GAP_STEP;

            if (decay >= BYTE_COUNTER_MASK || counter <= decay)
                counter = 0;
            else
                counter = static_cast<std::uint8_t>(counter - decay);
        }
        else
        {
            auto const current = (counter + (GAP_DECAY_THRESHOLD - gap) / GAP_STEP) & BYTE_COUNTER_MASK;

            counter = static_cast<std::uint8_t>(current);

            if (current > settings.density_cap)
                return true;
        }

        return false;
    }

    auto build_candidates(std::span<const event> chart, const int player)
        -> std::vector<placement_candidate>
    {
        auto const note_type = note_type_for_player(player);
        auto const sample_type = sample_type_for_player(player);

        auto last_sample_by_column = std::array<const event*, SAMPLE_SLOT_COUNT> {};
        last_sample_by_column.fill(nullptr);

        auto candidates = std::vector<placement_candidate> {};
        candidates.reserve(chart.size());
        auto first_bgm_seen = false;

        for (const auto& e: chart)
        {
            if (e.type == END_OF_SONG)
                break;

            if (e.type == BGM)
            {
                if (first_bgm_seen)
                    candidates.push_back(placement_candidate {
                        .offset = e.offset,
                        .sample_value = static_cast<std::uint16_t>(e.value),
                        .freeze_length = 0,
                        .lane_plus_one = 0,
                        .marker = BGM_CANDIDATE_MARKER
                    });
                else
                    first_bgm_seen = true;
            }
            else if (e.type == sample_type)
            {
                if (e.parameter >= 0 && e.parameter < SAMPLE_SLOT_COUNT)
                    last_sample_by_column[static_cast<std::size_t>(e.parameter)] = &e;
            }
            else if (e.type == note_type)
            {
                auto const parameter = e.parameter == LINKED_SCRATCH_LANE ? SCRATCH_LANE: e.parameter;

                if (parameter < 0 || parameter >= COLUMN_COUNT)
                    continue;

                auto const sample = last_sample_by_column[
                    static_cast<std::size_t>(parameter)];
                auto const sample_value = sample
                    ? static_cast<std::uint16_t>(sample->value)
                    : std::uint16_t { 0 };

                candidates.push_back(placement_candidate {
                    e.offset,
                    sample_value,
                    static_cast<std::uint16_t>(e.value),
                    static_cast<std::uint8_t>(parameter + 1),
                    ORIGINAL_NOTE_MARKER });
            }
        }

        return candidates;
    }

    auto count_baseline(std::span<const event> chart, const int player) -> int
    {
        auto count = 0;

        for (const auto& e: chart)
        {
            if (e.type == END_OF_SONG)
                break;

            if (e.type == note_type_for_player(player) && e.value != 0)
                ++count;
        }

        return count;
    }

    template<std::size_t KeyCount = PLAYABLE_LANE_COUNT>
    auto lane_priority_for_toggle(const int toggle) -> std::array<int, KeyCount>
    {
        static_assert(KeyCount == PLAYABLE_LANE_COUNT || KeyCount == DP_KEY_COUNT);
        auto const priority = toggle == 0
            ? HIGH_KEY_FIRST_PRIORITY
            : LOW_KEY_FIRST_PRIORITY;

        if constexpr (KeyCount == PLAYABLE_LANE_COUNT)
            return priority;
        else
        {
            auto combined = std::array<int, KeyCount> {};
            for (std::size_t index = 0; index < priority.size(); ++index)
            {
                combined[DP_SIDE_COUNT * index] = priority[index] + (toggle == 0 ? 0 : PLAYABLE_LANE_COUNT);
                combined[DP_SIDE_COUNT * index + 1] = priority[index] + (toggle == 0 ? PLAYABLE_LANE_COUNT : 0);
            }
            return combined;
        }
    }

    auto add_sample_occurrence(std::vector<weighted_sample>& working_set,
        const std::uint16_t sample_value) -> void
    {
        auto index = 0;
        auto const entry_count = static_cast<int>(working_set.size());

        while (index < entry_count && working_set[index].value != sample_value)
            ++index;

        if (index == entry_count)
        {
            working_set.insert(working_set.begin(), weighted_sample { sample_value, 1 });
            return;
        }

        if (working_set[index].occurrences == BYTE_COUNTER_MASK)
            return;

        working_set[index].occurrences += 1;

        for (auto k = index + 1; k < entry_count; ++k)
            if (working_set[k - 1].occurrences > working_set[k].occurrences)
                std::swap(working_set[k - 1], working_set[k]);
    }

    template<std::size_t KeyCount = PLAYABLE_LANE_COUNT>
    auto collect_group_samples(const std::vector<placement_candidate>& candidates,
        const int cursor, const int count, const std::int32_t span_ticks)
        -> std::pair<std::vector<weighted_sample>, int>
    {
        auto working_set = std::vector<weighted_sample> {};
        auto scan = cursor;
        auto const group_base_offset =
            KeyCount == PLAYABLE_LANE_COUNT && cursor < static_cast<int>(candidates.size())
                ? candidates[cursor].offset: 0;

        while (scan < count)
        {
            auto const& rec = candidates[scan];
            if (rec.offset - group_base_offset >= span_ticks)
                break;

            if (rec.lane_plus_one == 0)
                add_sample_occurrence(working_set, rec.sample_value);

            ++scan;
        }

        return { std::move(working_set), scan };
    }

    template<std::size_t KeyCount>
    auto score_lanes_for_sample(const std::vector<placement_candidate>& candidates,
        const int candidate_count,
        const std::uint16_t entry_value, const int entry_occurrences,
        const int threshold, const generation_settings& settings, lane_frame<KeyCount>& frame,
        std::array<std::uint8_t, KeyCount + 1>& lane_density,
        std::array<int, KeyCount + 1>& lane_accepts, const int candidate_start = 0,
        std::array<hand_workload, DP_SIDE_COUNT>* workloads = nullptr) -> bool
    {
        auto remaining = entry_occurrences;

        for (auto index = 0; index < candidate_count; ++index)
        {
            auto const& rec = candidates[index];

            if (rec.lane_plus_one != 0)
            {
                frame.record_real_note(rec);
                continue;
            }

            if (index < candidate_start || rec.sample_value != entry_value)
                continue;

            auto const record_offset = rec.offset;

            hand_activity activity;
            std::array<bool, DP_SIDE_COUNT> eligible {};
            if constexpr (KeyCount != PLAYABLE_LANE_COUNT)
                activity = hand_activity_at_tick(candidates, index);

            for (auto lane = 0; lane < static_cast<int>(KeyCount); ++lane)
            {
                if constexpr (KeyCount != PLAYABLE_LANE_COUNT)
                    if (rec.scratch_blocks(lane) ||
                        activity.occupied[lane / PLAYABLE_LANE_COUNT] >= settings.chord_cap)
                        continue;

                auto const free_from = frame.lane_free_from(lane);

                if (record_offset < free_from)
                    continue;

                auto const required_gap =
                    frame.lane_sample(lane) == entry_value ? SAME_SAMPLE_GAP * (KeyCount == PLAYABLE_LANE_COUNT ? 1 : DP_SPACING_MULTIPLIER): threshold;
                auto const gap = record_offset - free_from;

                if (gap < required_gap)
                    continue;

                if (density_limiter(gap, lane_density[lane], settings))
                    return true;

                auto const next_offset = next_note_offset_on_lane(candidates, index, lane);

                if (next_offset == 0 || (next_offset - record_offset) >= threshold)
                {
                    lane_accepts[lane] += 1;
                    frame.set_lane_free_from(lane, record_offset);
                    frame.set_lane_sample(lane, entry_value);
                    if constexpr (KeyCount != PLAYABLE_LANE_COUNT)
                        eligible[lane / PLAYABLE_LANE_COUNT] = true;
                }
            }

            if constexpr (KeyCount != PLAYABLE_LANE_COUNT)
            {
                if (workloads)
                    for (int side = 0; side < DP_SIDE_COUNT; ++side)
                        if (eligible[side])
                        {
                            (*workloads)[side].total += activity.workload(side);
                            ++(*workloads)[side].occurrences;
                        }
            }

            remaining -= 1;

            if (remaining == 0)
                break;
        }

        return false;
    }

    template<std::size_t SlotCount>
    auto favour_unused_lanes(std::array<int, SlotCount>& lane_accepts,
        const int last_placed_lane_plus_one) -> void
    {
        for (auto lane = 0; lane < static_cast<int>(SlotCount) - 1; ++lane)
            if (lane_accepts[lane] != 0 && (lane + 1) != last_placed_lane_plus_one)
                lane_accepts[lane] = (lane_accepts[lane] + 1) & BYTE_COUNTER_MASK;
    }

    template<std::size_t KeyCount>
    auto pick_target_lane(const std::array<int, KeyCount>& lane_priority,
        const std::array<int, KeyCount + 1>& lane_accepts) -> int
    {
        auto best_accepts = 0;
        auto best_lane_plus_one = 0;

        for (std::size_t slot = 0; slot < KeyCount; ++slot)
        {
            auto const lane = lane_priority[slot];
            auto const accepts = lane_accepts[lane];

            if (accepts > best_accepts)
            {
                best_accepts = accepts;
                best_lane_plus_one = lane + 1;
            }
        }

        return best_lane_plus_one;
    }

    template<std::size_t KeyCount>
    auto place_sample_on_lane(std::vector<placement_candidate>& candidates,
        const int candidate_count,
        const std::uint16_t entry_value, const int entry_occurrences,
        const int threshold, const int lane, const int lane_plus_one,
        lane_frame<KeyCount>& frame, const int candidate_start = 0,
        const int chord_cap = PLAYABLE_LANE_COUNT) -> int
    {
        auto placed = 0;
        auto remaining = entry_occurrences;

        for (auto index = 0; index < candidate_count; ++index)
        {
            auto& rec = candidates[index];

            if (rec.lane_plus_one != 0)
            {
                frame.record_real_note(rec);
                continue;
            }

            if (index < candidate_start || rec.sample_value != entry_value)
                continue;

            auto const record_offset = rec.offset;
            auto const free_from = frame.lane_free_from(lane);

            auto blocked = false;
            if constexpr (KeyCount != PLAYABLE_LANE_COUNT)
                blocked = rec.scratch_blocks(lane) ||
                    notes_per_hand_at_tick(candidates, index)[lane / PLAYABLE_LANE_COUNT] >= chord_cap;

            if (!blocked && record_offset >= free_from)
            {
                auto const required_gap =
                    frame.lane_sample(lane) == entry_value ? SAME_SAMPLE_GAP * (KeyCount == PLAYABLE_LANE_COUNT ? 1 : DP_SPACING_MULTIPLIER): threshold;

                if ((record_offset - free_from) >= required_gap)
                {
                    auto const next_offset = next_note_offset_on_lane(candidates, index, lane);

                    if (next_offset == 0 || (next_offset - record_offset) >= threshold)
                    {
                        frame.set_lane_sample(lane, entry_value);
                        frame.set_lane_free_from(lane, record_offset);
                        rec.lane_plus_one = static_cast<std::uint8_t>(lane_plus_one);
                        placed += 1;
                    }
                }
            }

            remaining -= 1;

            if (remaining == 0)
                break;
        }

        return placed;
    }

    template<std::size_t KeyCount = PLAYABLE_LANE_COUNT>
    auto generate_group_notes(generation_state& state, const std::int32_t span_ticks,
        const generation_settings& settings,
        std::vector<placement_candidate>& candidates) -> int
    {
        // DP's extra lanes allow more notes to be added - almost double if we use SP algorithm as-is.
        // To compensate for this, use 4x normal and repeated-keysound spacing to avoid adding too many.
        auto const threshold = settings.threshold * (KeyCount == PLAYABLE_LANE_COUNT ? 1 : DP_SPACING_MULTIPLIER);
        auto const lane_priority = lane_priority_for_toggle<KeyCount>(state.priority_toggle);
        auto const candidate_start = KeyCount == PLAYABLE_LANE_COUNT ? 0 : state.next_candidate;
        auto [working_set, scan] = collect_group_samples<KeyCount>(
            candidates, state.next_candidate, state.candidate_count, span_ticks);
        state.next_candidate = scan;
        auto const count = KeyCount == PLAYABLE_LANE_COUNT ? state.candidate_count : scan;

        auto placed_total = 0;
        auto last_placed_lane_plus_one = 0;

        for (auto entry = static_cast<int>(working_set.size()) - 1; entry >= 0; --entry)
        {
            auto const entry_value = working_set[entry].value;
            auto const entry_occurrences = working_set[entry].occurrences;

            auto frame = lane_frame<KeyCount> {};
            auto lane_density = std::array<std::uint8_t, KeyCount + 1> {};
            auto lane_accepts = std::array<int, KeyCount + 1> {};
            std::array<hand_workload, DP_SIDE_COUNT> workloads {};

            auto const capped = score_lanes_for_sample(candidates, count,
                entry_value, entry_occurrences, threshold, settings, frame,
                lane_density, lane_accepts, candidate_start, &workloads);

            if (capped)
                continue;

            favour_unused_lanes(lane_accepts, last_placed_lane_plus_one);
            if constexpr (KeyCount != PLAYABLE_LANE_COUNT)
                prefer_less_busy_hand(lane_accepts, workloads);

            auto const lane_plus_one = pick_target_lane(lane_priority, lane_accepts);

            if (lane_plus_one == 0)
                continue;

            last_placed_lane_plus_one = lane_plus_one;

            if constexpr (KeyCount != PLAYABLE_LANE_COUNT)
                frame = {};

            placed_total += place_sample_on_lane(candidates, count, entry_value,
                entry_occurrences, threshold, lane_plus_one - 1, lane_plus_one, frame, candidate_start,
                settings.chord_cap);
        }

        state.priority_toggle ^= 1;

        return placed_total;
    }

    template<std::size_t KeyCount = PLAYABLE_LANE_COUNT>
    auto run_generator(std::vector<placement_candidate>& candidates,
        std::span<const event> chart, const generation_settings& settings) -> int
    {
        auto state = generation_state {};
        state.candidate_count = static_cast<int>(candidates.size());

        auto bars_in_group = 0;
        auto placed_total = 0;
        auto group_start_offset = std::int32_t { 0 };
        auto eos_offset = std::int32_t { 0 };
        auto last_bar = std::int32_t { -1 };

        for (const auto& e: chart)
        {
            if (e.type == END_OF_SONG)
            {
                eos_offset = e.offset;
                break;
            }

            if (e.type == MEASURE_BAR)
            {
                if constexpr (KeyCount != PLAYABLE_LANE_COUNT)
                {
                    if (e.offset == last_bar)
                        continue;
                    last_bar = e.offset;
                }
                ++bars_in_group;

                if (bars_in_group == settings.measure_interval)
                {
                    placed_total += generate_group_notes<KeyCount>(state,
                        KeyCount == PLAYABLE_LANE_COUNT ? e.offset - group_start_offset : e.offset,
                        settings, candidates);
                    group_start_offset = e.offset;
                    bars_in_group = 0;
                }
            }
        }

        placed_total += generate_group_notes<KeyCount>(state,
            KeyCount == PLAYABLE_LANE_COUNT ? eos_offset - group_start_offset : eos_offset,
            settings, candidates);

        return placed_total;
    }

    auto append_generated_events(std::vector<event>& out,
        const std::vector<placement_candidate>& candidates, const int player,
        const bool mute_bgm) -> void
    {
        auto const sample_type = sample_type_for_player(player);
        auto const note_type = note_type_for_player(player);

        auto lane_free_from = std::array<std::int32_t, COLUMN_COUNT> {};
        auto lane_sample = std::array<std::int32_t, COLUMN_COUNT> {};
        lane_sample.fill(-1);

        for (const auto& rec: candidates)
        {
            auto const lane_plus_one = rec.lane_plus_one;

            if (lane_plus_one == 0)
            {
                if (!mute_bgm)
                    out.push_back(event { rec.offset, BGM, 0,
                        static_cast<std::int16_t>(rec.sample_value) });

                continue;
            }

            auto const lane = lane_plus_one - 1;

            if (lane == static_cast<int>(scratch))
                continue;

            if (lane_sample[lane] != static_cast<std::int32_t>(rec.sample_value))
            {
                auto const free_from = lane_free_from[lane];
                auto const midpoint = free_from + ((rec.offset - free_from) >> 1);

                out.push_back(event { midpoint, sample_type,
                    static_cast<std::int8_t>(lane),
                    static_cast<std::int16_t>(rec.sample_value) });

                lane_sample[lane] = static_cast<std::int32_t>(rec.sample_value);
            }

            out.push_back(event { rec.offset, note_type,
                static_cast<std::int8_t>(lane),
                static_cast<std::int16_t>(rec.freeze_length) });

            lane_free_from[lane] = rec.offset + rec.freeze_length;
        }
    }

    auto convert_dp_events(std::span<const event> chart, const bool kichiku) -> std::vector<event>
    {
        auto const settings = generation_settings { kichiku };
        auto candidates = std::vector<placement_candidate> {};
        for (auto player = 0; player < DP_SIDE_COUNT; ++player)
        {
            for (auto candidate : build_candidates(chart, player))
            {
                if (candidate.lane_plus_one > PLAYABLE_LANE_COUNT ||
                    (player != 0 && candidate.lane_plus_one == 0))
                    continue;
                if (candidate.lane_plus_one != 0)
                    candidate.lane_plus_one += static_cast<std::uint8_t>(player * PLAYABLE_LANE_COUNT);
                candidates.push_back(candidate);
            }
        }
        std::stable_sort(candidates.begin(), candidates.end(), [](const placement_candidate& left,
                                                               const placement_candidate& right) {
            return left.offset < right.offset;
        });
        mark_dp_scratch_exclusions(candidates, chart, settings.scratch_margin);
        run_generator<DP_KEY_COUNT>(candidates, chart, settings);

        auto totals = std::array<int, DP_SIDE_COUNT> {};
        auto player_candidates = std::array<std::vector<placement_candidate>, DP_SIDE_COUNT> {};
        for (auto candidate : candidates)
        {
            if (candidate.lane_plus_one == 0)
            {
                player_candidates[0].push_back(candidate);
                continue;
            }
            auto const lane = candidate.lane_plus_one - 1;
            auto const player = lane / PLAYABLE_LANE_COUNT;
            if (candidate.marker == BGM_CANDIDATE_MARKER)
                ++totals[player];
            candidate.lane_plus_one = static_cast<std::uint8_t>(lane % PLAYABLE_LANE_COUNT + 1);
            player_candidates[player].push_back(candidate);
        }

        auto out = std::vector<event> {};
        auto first_bgm_seen = false;
        for (auto record : chart)
        {
            if ((record.type == NOTE_P1 || record.type == NOTE_P2 ||
                 record.type == SAMPLE_P1 || record.type == SAMPLE_P2) &&
                record.parameter >= 0 && record.parameter < PLAYABLE_LANE_COUNT)
                continue;
            if (record.type == BGM)
            {
                if (first_bgm_seen)
                    continue;
                first_bgm_seen = true;
            }
            if (record.type == NOTE_COUNT && record.parameter >= 0 && record.parameter < DP_SIDE_COUNT)
                record.value = static_cast<std::int16_t>(record.value + totals[record.parameter]);
            out.push_back(record);
        }

        for (auto player = 0; player < DP_SIDE_COUNT; ++player)
            append_generated_events(out, player_candidates[player], player, player != 0);

        std::stable_sort(out.begin(), out.end(), [](const event& left, const event& right)
        {
            if (left.offset != right.offset)
                return left.offset < right.offset;
            auto const order = [](const event& record)
            {
                return record.type == END_OF_SONG ? 2 :
                    (record.type == NOTE_P1 || record.type == NOTE_P2 ? 1 : 0);
            };
            return order(left) < order(right);
        });
        return out;
    }

    auto sort_by_offset(std::vector<event>& events) -> void
    {
        if (events.size() < 2)
            return;

        std::reverse(events.begin() + 1, events.end());
        std::stable_sort(events.begin() + 1, events.end(),
            [] (const event& a, const event& b) { return a.offset < b.offset; });
    }

    auto convert_buffer(std::uint8_t* buffer, const std::size_t capacity,
        const int player, const bool kichiku, const bool double_play) -> std::size_t
    {
        if (buffer == nullptr || capacity < urafumen_experimental::EVENT_SIZE)
            return 0;

        auto parsed = urafumen_experimental::parse_events_until_eos({ buffer, capacity });
        if (!parsed)
            return 0;  // malformed / truncated chart; leave the buffer untouched

        if (double_play && !std::is_sorted(parsed->begin(), parsed->end(),
            [] (const event& left, const event& right) { return left.offset < right.offset; }))
            return 0;

        auto const converted = double_play ? convert_dp_events(*parsed, kichiku) :
            urafumen_experimental::convert(*parsed, player, kichiku).events;
        auto const written = converted.size() * urafumen_experimental::EVENT_SIZE;
        if (written > capacity)
            return 0;  // converted chart would overflow the buffer

        auto* cursor = buffer;
        for (auto const& record : converted)
        {
            bm2dx::write_chart_event(cursor, record);
            cursor += urafumen_experimental::EVENT_SIZE;
        }
        return written;
    }
}

auto urafumen_experimental::sha256_hex(const std::uint8_t* data, const std::size_t len)
    -> std::string
{
    std::string out;
    picosha2::hash256_hex_string(data, data + len, out);
    return out;
}

auto urafumen_experimental::parse_events_until_eos(std::span<const std::uint8_t> data)
    -> std::optional<std::vector<event>>
{
    auto chart = std::vector<event> {};
    chart.reserve(data.size() / EVENT_SIZE);
    auto offset = std::size_t { 0 };
    auto saw_eos = false;

    while (offset + EVENT_SIZE <= data.size())
    {
        auto const e = bm2dx::read_chart_event(data.data() + offset);

        if (e.offset == 0 && e.type == NOTE_P1 && e.parameter == 0 && e.value == 0)
            break;

        chart.push_back(e);
        offset += EVENT_SIZE;

        if (e.type == END_OF_SONG)
        {
            saw_eos = true;
            break;
        }
    }

    if (!saw_eos)
        return std::nullopt;

    return chart;
}

auto urafumen_experimental::parse_events(std::span<const std::uint8_t> data)
    -> std::vector<event>
{
    auto events = std::vector<event> {};
    events.reserve(data.size() / EVENT_SIZE);
    auto offset = std::size_t { 0 };

    while (offset + EVENT_SIZE <= data.size())
    {
        events.push_back(bm2dx::read_chart_event(data.data() + offset));
        offset += EVENT_SIZE;
    }

    return events;
}

auto urafumen_experimental::pack_events(std::span<const event> events)
    -> std::vector<std::uint8_t>
{
    auto out = std::vector<std::uint8_t>(events.size() * EVENT_SIZE);
    auto* cursor = out.data();

    for (const auto& e: events)
    {
        bm2dx::write_chart_event(cursor, e);
        cursor += EVENT_SIZE;
    }

    return out;
}

auto urafumen_experimental::convert(std::span<const event> chart, const int player,
    const bool kichiku) -> result
{
    auto const settings = generation_settings { kichiku };
    auto candidates = build_candidates(chart, player);
    auto const baseline = count_baseline(chart, player);
    auto const generated = run_generator(candidates, chart, settings);

    auto const note_type = note_type_for_player(player);
    auto const sample_type = sample_type_for_player(player);

    auto out = std::vector<event> {};
    out.reserve(chart.size() + candidates.size() + 2);
    auto first_bgm_seen = false;
    auto eos_offset = UNKNOWN_END_OFFSET;

    auto const n = static_cast<int>(chart.size());

    for (auto idx = 0; idx < n; ++idx)
    {
        const auto& e = chart[idx];

        if (e.type == END_OF_SONG)
        {
            eos_offset = e.offset;
            break;
        }

        if (e.type == note_type || e.type == sample_type)
        {
            if (e.parameter == SCRATCH_LANE || e.parameter == LINKED_SCRATCH_LANE)
                out.push_back(e);
        }
        else if (e.type == BGM)
        {
            if (!first_bgm_seen)
            {
                first_bgm_seen = true;
                out.push_back(e);
            }
        }
        else if (e.type == NOTE_COUNT &&
            e.parameter == static_cast<std::int8_t>(note_type))
        {
            out.push_back(event { e.offset, e.type, e.parameter,
                static_cast<std::int16_t>(e.value + generated) });
        }
        else
        {
            out.push_back(e);
        }
    }

    append_generated_events(out, candidates, player, false);
    out.push_back(event { eos_offset, END_OF_SONG, 0, 0 });

    sort_by_offset(out);

    return result { std::move(out), baseline, generated };
}

auto urafumen_experimental::convert_in_place(std::uint8_t* buffer, const std::size_t capacity,
    const int player, const bool kichiku) -> std::size_t
{
    return convert_buffer(buffer, capacity, player, kichiku, false);
}

auto urafumen_experimental::convert_dp_in_place(std::uint8_t* buffer, const std::size_t capacity,
    const bool kichiku) -> std::size_t
{
    return convert_buffer(buffer, capacity, 0, kichiku, true);
}
