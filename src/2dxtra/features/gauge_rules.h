#pragma once

#include <array>
#include <optional>

namespace iidxtra::gauge_rules
{
    // Signed changes for PGREAT, GREAT, GOOD, BAD, missed POOR, and empty POOR.
    using judgment_deltas = std::array<int, 6>;

    struct erosion_calculation
    {
        // Gauge percentage lost at each interval.
        double drain_percent;

        // Native ticks between drains.
        int interval_ticks;

        // Final tick at which a drain can occur.
        int end_tick;

    };

    // Unsupported judgments or levels return no delta; the caller handles the error.
    auto hazard_delta(int judgment, int gauge_value, const judgment_deltas& normal_deltas) -> std::optional<int>;
    auto ex_dan_delta(int judgment) -> std::optional<int>;
    auto erosion_delta(int level, int judgment) -> std::optional<int>;

    // Invalid chart timing or level returns no calculation.
    auto calculate_erosion(int level, double notes, double duration_seconds,
                           float tick_ms, bool double_play) -> std::optional<erosion_calculation>;
    auto erosion_drain_delta(double drain_percent, int gauge_value) -> int;
}
