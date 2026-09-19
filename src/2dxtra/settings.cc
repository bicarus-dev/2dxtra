#include <meta.h>
#include <algorithm>
#include <cmath>
#include <limits>
#include <type_traits>
#include "settings.h"
#include "log.h"
#include "gui/gui.h"
#include "features/autoplay.h"
#include "features/autoretry.h"
#include "features/chart_speed.h"
#include "features/cn_override.h"
#include "features/cn_transformer.h"
#include "features/fast_slow_display.h"
#include "features/keysound_switch.h"
#include "features/play_visuals.h"
#include "features/regular_speed.h"
#include "features/scratch_swap.h"
#include "features/timing_histogram.h"
#include "features/timing_modifier.h"
#include "features/unrandomizer.h"

namespace iidxtra::settings
{
    static database::db* storage = nullptr;

    template <typename Visitor>
    static auto visit_timing(Visitor& visit, const std::string& prefix,
                             bm2dx::timing_t& timing) -> void
    {
        constexpr auto limit = std::numeric_limits<float>::max();
        visit(prefix + ".early_poor", timing.early_poor, -limit, limit);
        visit(prefix + ".early_bad", timing.early_bad, -limit, limit);
        visit(prefix + ".early_good", timing.early_good, -limit, limit);
        visit(prefix + ".early_great", timing.early_great, -limit, limit);
        visit(prefix + ".early_pgreat", timing.early_pgreat, -limit, limit);
        visit(prefix + ".late_pgreat", timing.late_pgreat, -limit, limit);
        visit(prefix + ".late_great", timing.late_great, -limit, limit);
        visit(prefix + ".late_good", timing.late_good, -limit, limit);
        visit(prefix + ".late_bad", timing.late_bad, -limit, limit);
    }

    template <typename Visitor>
    static auto visit_settings(Visitor visit) -> void
    {
        visit("interface.menu_lock", gui::play_lock_state);
        visit("settings.event_mode", gui::config_event_mode);
        visit("autoplay.p1", autoplay::enabled_p1);
        visit("autoplay.p2", autoplay::enabled_p2);
        visit("regular_speed.enabled", regular_speed::enabled);
        visit("chart_speed.rate", chart_speed::rate, chart_speed::rate_min, chart_speed::rate_max);
        visit("chart_speed.previous", chart_speed::rate_previous, chart_speed::rate_min, chart_speed::rate_max);
        visit("keysound.mode", keysound_switch::override_type, 0, 3);
        visit("keysound.mute_bgm", keysound_switch::mute_bgm);
        visit("visuals.dark_mode", play_visuals::dark_mode);
        visit("visuals.no_measure_lines", play_visuals::no_measure_lines);
        visit("visuals.no_bpm_gradient", play_visuals::no_bpm_gradient);
        visit("visuals.concentration_movie", play_visuals::concentration_movie);
        visit("visuals.subscreen_dim_level", play_visuals::subscreen_dim_level, 0, 4);
        visit("fast_slow.milliseconds", fast_slow_display::options.show_milliseconds);
        visit("fast_slow.pgreat", fast_slow_display::options.show_pgreat);
        visit("timing_histogram.enabled", timing_histogram::enabled);
        visit("autoretry.enabled", autoretry::enabled);
        visit("autoretry.target", autoretry::target, 0, 2);
        visit("autoretry.destination", autoretry::destination, 0, 1);

        for (auto player = 0; player < 2; ++player)
        {
            auto const prefix = "p" + std::to_string(player + 1) + ".";
            visit(prefix + "cn_override", player == 0 ? cn_override::p1_override_type : cn_override::p2_override_type, 0, 2);
            visit(prefix + "cn.enabled", player == 0 ? cn_transformer::enabled_p1 : cn_transformer::enabled_p2);
            visit(prefix + "cn.percentage", player == 0 ? cn_transformer::long_note_percentage_p1 : cn_transformer::long_note_percentage_p2, 5, 100);
            visit(prefix + "cn.duration", player == 0 ? cn_transformer::max_cn_duration_p1 : cn_transformer::max_cn_duration_p2, -1, 10000);
            visit(prefix + "cn.scratch", player == 0 ? cn_transformer::allow_backspin_scratch_p1 : cn_transformer::allow_backspin_scratch_p2);
            visit(prefix + "random.enabled", player == 0 ? unrandomizer::enabled_p1 : unrandomizer::enabled_p2);
            visit(prefix + "random.save_scores", player == 0 ? unrandomizer::allow_score_save_p1 : unrandomizer::allow_score_save_p2);
            visit(prefix + "random.show_info", player == 0 ? unrandomizer::show_random_info_p1 : unrandomizer::show_random_info_p2);
            visit(prefix + "scratch_swap.enabled", player == 0 ? scratch_swap::enabled_p1 : scratch_swap::enabled_p2);
            visit(prefix + "scratch_swap.lane", player == 0 ? scratch_swap::swap_lane_p1 : scratch_swap::swap_lane_p2, -1, 6);

            auto& columns = player == 0 ? unrandomizer::column_lut_p1 : unrandomizer::column_lut_p2;
            for (auto lane = std::size_t { 0 }; lane < columns.size(); ++lane)
                visit(prefix + "random.lane" + std::to_string(lane), columns[lane], 0, 6);

            visit(prefix + "timing.keys", timing_modifier::enabled_keys[player]);
            visit(prefix + "timing.scratch", timing_modifier::enabled_scratch[player]);
            visit(prefix + "timing.copy_from_chart", timing_modifier::copy_from_chart[player]);
            visit_timing(visit, prefix + "timing.keys", timing_modifier::override[player].keys);
            visit_timing(visit, prefix + "timing.scratch", timing_modifier::override[player].scratch);
        }
    }

    auto init(database::db* db) -> void
    {
        storage = db;
        auto const values = database::load_settings(storage);
        if (!values)
        {
            log::init("Could not load settings; using defaults");
            return;
        }

        visit_settings([&](const std::string& key, auto& setting, double minimum = 0, double maximum = 1)
        {
            auto const found = std::ranges::find(*values, key, &database::settings_t::value_type::first);
            if (found == values->end())
                return;

            auto const value = found->second;
            if (!std::isfinite(value) || value < minimum || value > maximum)
                return;

            using value_type = std::remove_reference_t<decltype(setting)>;
            if constexpr (!std::is_floating_point_v<value_type>)
            {
                if (std::trunc(value) != value)
                    return;
            }
            setting = static_cast<value_type>(value);
        });

        for (auto player = std::uint8_t { 0 }; player < 2; ++player)
        {
            if (!unrandomizer::is_valid(player))
                (player == 0 ? unrandomizer::column_lut_p1 : unrandomizer::column_lut_p2) = { 0, 1, 2, 3, 4, 5, 6 };
            if (timing_modifier::enabled_keys[player] || timing_modifier::enabled_scratch[player])
                timing_modifier::copy_from_chart[player] = false;
        }

        #if FORCE_EVENT_MODE_ENABLED == 1
        gui::config_event_mode = true;
        #endif
        #if BLOCK_FORCE_RANDOM_SAVE == 1
        unrandomizer::allow_score_save_p1 = false;
        unrandomizer::allow_score_save_p2 = false;
        #endif

        play_visuals::update_dark_mode();
        play_visuals::update_no_measure_lines();
        play_visuals::update_no_bpm_gradient();
        play_visuals::update_concentration_movie();
        play_visuals::update_subscreen_dim();
        fast_slow_display::update();
        timing_histogram::update();
    }

    auto save() -> bool
    {
        database::settings_t values;
        visit_settings([&](const std::string& key, const auto& setting, double = 0, double = 1)
        {
            values.emplace_back(key, static_cast<double>(setting));
        });
        return database::save_settings(storage, values);
    }

    auto reset(const bool include_event_mode) -> void
    {
        gui::play_lock_state = false;
        if (include_event_mode)
            gui::config_event_mode = FORCE_EVENT_MODE_ENABLED != 0;

        autoplay::reset();
        cn_override::reset();
        regular_speed::reset();
        chart_speed::reset();
        scratch_swap::reset();
        unrandomizer::reset();
        keysound_switch::reset();
        cn_transformer::reset();
        play_visuals::reset();
        timing_modifier::reset();
        fast_slow_display::reset();
        timing_histogram::reset();
        autoretry::reset();
    }
}