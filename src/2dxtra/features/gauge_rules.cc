#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include "gauge_rules.h"

namespace iidxtra::gauge_rules
{
    namespace
    {
        struct erosion_parameters
        {
            gauge_rules::judgment_deltas deltas;
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
        constexpr judgment_deltas ex_dan_judgment_deltas {8, 8, 2, -170, -250, -170};

        auto density_factor(float density, bool double_play) -> float
        {
            // INFINITAS uses separate SP/DP density breakpoints.
            const auto thresholds = double_play ? std::array {10.4f, 15.4f, 17.7f, 20.0f} :
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
    }

    auto hazard_delta(int judgment, int gauge_value, const judgment_deltas& normal_deltas) -> std::optional<int>
    {
        if (judgment < 0 || judgment >= 6)
            return std::nullopt;

        // BAD and missed-note POOR fail immediately.
        if (judgment == 3 || judgment == 4)
            return -gauge_value;

        // Recovery and empty POOR use Normal's signed deltas.
        return normal_deltas[judgment];
    }

    auto ex_dan_delta(int judgment) -> std::optional<int>
    {
        if (judgment < 0 || judgment >= 6)
            return std::nullopt;

        return ex_dan_judgment_deltas[judgment];
    }

    auto erosion_delta(int level, int judgment) -> std::optional<int>
    {
        if (level < 1 || level > 5 || judgment < 0 || judgment >= 6)
            return std::nullopt;

        return erosion_levels[level - 1].deltas[judgment];
    }

    auto calculate_erosion(int level, double notes, double duration_seconds,
                           float tick_ms, bool double_play) -> std::optional<erosion_calculation>
    {
        if (level < 1 || level > 5 ||
            !std::isfinite(notes) || notes <= 0 ||
            !std::isfinite(duration_seconds) || duration_seconds <= 0 ||
            !std::isfinite(tick_ms) || tick_ms <= 0)
            return std::nullopt;

        const auto& parameters = erosion_levels[level - 1];
        const auto density = static_cast<float>(notes / duration_seconds);
        const int factor = static_cast<int>(density_factor(density, double_play) * 10000.0f);
        const auto base_drain = (notes * 8.0 * static_cast<double>(0.16f) / 9.0 + 50.0) /
            (notes * 1000.0);
        const auto drain_percent = factor * (base_drain * parameters.drain_multiplier);
        const auto interval_ticks = parameters.interval_seconds * 1000.0 / tick_ms + 0.5;
        const auto end_tick = duration_seconds * 1000.0 / tick_ms;

        if (!std::isfinite(drain_percent) || drain_percent < 0 ||
            drain_percent > std::numeric_limits<int>::max() / 50.0 ||
            interval_ticks > std::numeric_limits<int>::max() ||
            end_tick > std::numeric_limits<int>::max())
            return std::nullopt;

        return erosion_calculation {
            drain_percent,
            std::max(1, static_cast<int>(interval_ticks)),
            static_cast<int>(end_tick)
        };
    }

    auto erosion_drain_delta(double drain_percent, int gauge_value) -> int
    {
        const auto drain = gauge_value < 1666 ? drain_percent * 0.5 : drain_percent;
        return -static_cast<int>(drain * 50.0);
    }
}
