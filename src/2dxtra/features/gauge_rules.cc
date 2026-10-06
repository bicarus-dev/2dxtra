#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include "gauge_rules.h"

namespace iidxtra::gauge_rules
{
    auto calculate_lr2(gauge::type mode, int notes) -> std::optional<lr2_parameters>
    {
        if (notes <= 0 || !gauge::is_lr2(mode))
            return std::nullopt;

        // Class-course Dan recovery and damage are fixed, independent of TOTAL.
        if (mode == gauge::type::LR2Dan)
            return lr2_parameters {{0.1, 0.1, 0.04, -2.0, -3.0, -2.0}, false};

        // OpenLR2 LR2_bmsload.cpp, revision 49b1865b98187b7f17a95c5154c55ea6ffbc54c4.
        // TOTAL is an integer; the per-note division is evaluated as float.
        const double base_total = notes < 400 ? notes / 5.0 + 200.0 :
            notes < 600 ? (notes - 400) / 2.5 + 280.0 : (notes - 600) / 5.0 + 360.0;
        const int total = static_cast<int>(base_total * 0.8);
        const double recovery = total / static_cast<float>(notes);
        const double good_recovery = total / (static_cast<float>(notes) * 2.0f);

        double note_damage;
        if (notes < 20) note_damage = 10.0;
        else if (notes < 30) note_damage = 10.0 - (notes - 20.0) / 10.0 * 2.0;
        else if (notes < 45) note_damage = 7.0 - (notes - 30.0) / 15.0;
        else if (notes < 60) note_damage = 6.0 - (notes - 45.0) / 15.0;
        else if (notes < 125) note_damage = 5.0 - (notes - 60.0) / 65.0;
        else if (notes < 250) note_damage = 4.0 - (notes - 125.0) / 125.0;
        else if (notes < 500) note_damage = 3.0 - (notes - 250.0) / 250.0;
        else if (notes < 1000) note_damage = 2.0 - (notes - 500.0) / 500.0;
        else note_damage = 1.0;

        const int recover = std::max(1, static_cast<int>((total - 80.0) * 0.125 / 2));
        const double damage = std::max(note_damage * 10, 100.0 / recover) / 10.0;
        switch (mode)
        {
            case gauge::type::LR2Easy:
                return lr2_parameters {{recovery * 1.2, recovery * 1.2, good_recovery * 1.2,
                    -3.2, -4.800000000000001, -1.6}, true};
            case gauge::type::LR2Normal:
                return lr2_parameters {{recovery, recovery, good_recovery, -4, -6, -2}, true};
            case gauge::type::LR2Hard:
                return lr2_parameters {{0.1, 0.1, 0.05, -6 * damage, -10 * damage, -2 * damage}, false};
            case gauge::type::LR2Death:
                return lr2_parameters {{0, 0, 0, -100, -100, 0}, false};
            case gauge::type::LR2PAttack:
                return lr2_parameters {{0.1, -1, -100, -100, -100, -100}, false};
            case gauge::type::LR2GAttack:
                return lr2_parameters {{-10 * damage, -1, 0.1, -6, -10 * damage, -2 * damage}, false};
            default:
                return std::nullopt;
        }
    }

    auto lr2_value(const lr2_parameters& parameters, int judgment, double percent) -> std::optional<double>
    {
        if (judgment < 0 || judgment >= 6 || !std::isfinite(percent) || percent < 0 || percent > 100)
            return std::nullopt;
        if (!parameters.recovery && percent < 2)
            return 0;

        auto delta = parameters.deltas[judgment];
        // LR2 tests the displayed even percentage: 30% includes actual values below 32%.
        if (!parameters.recovery && percent < 32 && judgment >= 3)
            delta *= 0.6;

        return std::clamp(percent + delta, parameters.recovery ? 2.0 : 0.0, 100.0);
    }

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
