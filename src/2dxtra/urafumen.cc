#include <array>
#include <algorithm>
#include <optional>
#include <string>
#include <picosha2.h>

#include "urafumen.h"

namespace
{
    using urafumen::event;
    using urafumen::event_type;
    using urafumen::column;
    using enum event_type;
    using enum column;

    auto constexpr NO_VALUE = std::uint16_t { 0xFFFF };

    auto constexpr GAP_DECAY_THRESHOLD = 350;
    auto constexpr GAP_STEP = 10;
    auto constexpr SAME_SAMPLE_GAP = 80;

    auto constexpr PLAYABLE_LANE_COUNT = 7;
    auto constexpr COLUMN_COUNT = 8;
    auto constexpr SAMPLE_SLOT_COUNT = 9;

    struct generation_settings
    {
        int threshold;
        int density_cap;
        int measure_interval;

        explicit generation_settings(const bool kichiku):
            threshold { kichiku ? 200: 250 },
            density_cap { kichiku ? 100: 50 },
            measure_interval { kichiku ? 4: 8 } {}
    };

    struct placement_candidate
    {
        std::int32_t offset = 0;
        std::uint16_t sample_value = 0;
        std::uint16_t freeze_length = 0;
        std::uint8_t lane_plus_one = 0;
        std::uint8_t marker = 0;
    };

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
        { return static_cast<event_type>(static_cast<int>(note_type_for_player(player)) + 2); }

    auto density_limiter(const int gap, std::uint8_t& counter,
        const generation_settings& settings) -> bool
    {
        if (gap >= GAP_DECAY_THRESHOLD)
        {
            auto const decay = (gap - GAP_DECAY_THRESHOLD) / GAP_STEP;

            if (decay >= 0xFF || counter <= decay)
                counter = 0;
            else
                counter = static_cast<std::uint8_t>(counter - decay);
        }
        else
        {
            auto const current = (counter + (GAP_DECAY_THRESHOLD - gap) / GAP_STEP) & 0xFF;

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
                        .marker = 0xCC
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
                auto const parameter = e.parameter == 107 ? 7: e.parameter;

                if (parameter < 0 || parameter >= 8)
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
                    0xAA });
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
        static_assert(KeyCount == PLAYABLE_LANE_COUNT || KeyCount == 2 * PLAYABLE_LANE_COUNT);
        auto const priority = toggle == 0
            ? std::array { 6, 4, 2, 0, 5, 1, 3 }
            : std::array { 0, 2, 4, 6, 1, 5, 3 };

        if constexpr (KeyCount == PLAYABLE_LANE_COUNT)
            return priority;
        else
        {
            auto combined = std::array<int, KeyCount> {};
            for (std::size_t index = 0; index < priority.size(); ++index)
            {
                combined[2 * index] = priority[index] + (toggle == 0 ? 0 : PLAYABLE_LANE_COUNT);
                combined[2 * index + 1] = priority[index] + (toggle == 0 ? PLAYABLE_LANE_COUNT : 0);
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

        if (working_set[index].occurrences == 0xFF)
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
        std::array<int, KeyCount + 1>& lane_accepts, const int candidate_start = 0) -> bool
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

            for (auto lane = 0; lane < static_cast<int>(KeyCount); ++lane)
            {
                auto const free_from = frame.lane_free_from(lane);

                if (record_offset < free_from)
                    continue;

                auto const required_gap =
                    frame.lane_sample(lane) == entry_value ? SAME_SAMPLE_GAP: threshold;
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
                lane_accepts[lane] = (lane_accepts[lane] + 1) & 0xFF;
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
        lane_frame<KeyCount>& frame, const int candidate_start = 0) -> int
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

            if (record_offset >= free_from)
            {
                auto const required_gap =
                    frame.lane_sample(lane) == entry_value ? SAME_SAMPLE_GAP: threshold;

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
        auto const threshold = settings.threshold;
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

            auto const capped = score_lanes_for_sample(candidates, count,
                entry_value, entry_occurrences, threshold, settings, frame,
                lane_density, lane_accepts, candidate_start);

            if (capped)
                continue;

            favour_unused_lanes(lane_accepts, last_placed_lane_plus_one);

            auto const lane_plus_one = pick_target_lane(lane_priority, lane_accepts);

            if (lane_plus_one == 0)
                continue;

            last_placed_lane_plus_one = lane_plus_one;

            if constexpr (KeyCount != PLAYABLE_LANE_COUNT)
                frame = {};

            placed_total += place_sample_on_lane(candidates, count, entry_value,
                entry_occurrences, threshold, lane_plus_one - 1, lane_plus_one, frame, candidate_start);
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
        for (auto player = 0; player < 2; ++player)
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
        run_generator<2 * PLAYABLE_LANE_COUNT>(candidates, chart, settings);

        auto totals = std::array<int, 2> {};
        auto player_candidates = std::array<std::vector<placement_candidate>, 2> {};
        for (auto candidate : candidates)
        {
            if (candidate.lane_plus_one == 0)
            {
                player_candidates[0].push_back(candidate);
                continue;
            }
            auto const lane = candidate.lane_plus_one - 1;
            auto const player = lane / PLAYABLE_LANE_COUNT;
            if (candidate.marker == 0xCC)
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
            if (record.type == NOTE_COUNT && record.parameter >= 0 && record.parameter < 2)
                record.value = static_cast<std::int16_t>(record.value + totals[record.parameter]);
            out.push_back(record);
        }

        for (auto player = 0; player < 2; ++player)
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
        if (buffer == nullptr || capacity < urafumen::EVENT_SIZE)
            return 0;

        auto parsed = urafumen::parse_events_until_eos({ buffer, capacity });
        if (!parsed)
            return 0;  // malformed / truncated chart; leave the buffer untouched

        if (double_play && !std::is_sorted(parsed->begin(), parsed->end(),
            [] (const event& left, const event& right) { return left.offset < right.offset; }))
            return 0;

        auto const converted = double_play ? convert_dp_events(*parsed, kichiku) :
            urafumen::convert(*parsed, player, kichiku).events;
        auto const written = converted.size() * urafumen::EVENT_SIZE;
        if (written > capacity)
            return 0;  // converted chart would overflow the buffer

        auto* cursor = buffer;
        for (auto const& record : converted)
        {
            bm2dx::write_chart_event(cursor, record);
            cursor += urafumen::EVENT_SIZE;
        }
        return written;
    }
}

auto urafumen::sha256_hex(const std::uint8_t* data, const std::size_t len)
    -> std::string
{
    std::string out;
    picosha2::hash256_hex_string(data, data + len, out);
    return out;
}

auto urafumen::parse_events_until_eos(std::span<const std::uint8_t> data)
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

auto urafumen::parse_events(std::span<const std::uint8_t> data)
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

auto urafumen::pack_events(std::span<const event> events)
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

auto urafumen::convert(std::span<const event> chart, const int player,
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
    auto eos_offset = std::int32_t { 0x7FFFFFFF };

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
            if (e.parameter == 7 || e.parameter == 107)
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

auto urafumen::convert_in_place(std::uint8_t* buffer, const std::size_t capacity,
    const int player, const bool kichiku) -> std::size_t
{
    return convert_buffer(buffer, capacity, player, kichiku, false);
}

auto urafumen::convert_dp_in_place(std::uint8_t* buffer, const std::size_t capacity,
    const bool kichiku) -> std::size_t
{
    return convert_buffer(buffer, capacity, 0, kichiku, true);
}
