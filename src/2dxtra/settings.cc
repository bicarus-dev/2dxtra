#include <meta.h>
#include <cmath>
#include <limits>
#include <type_traits>
#include "settings.h"
#include "gui/gui.h"
#include "hooks/reset_state_hook.h"
#include "features/audio_balance.h"
#include "features/custom_audio.h"
#include "features/autoplay.h"
#include "features/autoretry.h"
#include "features/chart_speed.h"
#include "features/cn_override.h"
#include "features/cn_transformer.h"
#include "features/fast_slow_display.h"
#include "features/gauge.h"
#include "features/note_colors.h"
#include "features/hi_speed_reset.h"
#include "features/keysound_switch.h"
#include "features/play_visuals.h"
#include "features/regular_speed.h"
#include "features/scratch_swap.h"
#include "features/scratch_flip.h"
#include "features/timing_histogram.h"
#include "features/live_timing.h"
#include "features/timing_modifier.h"
#include "features/unrandomizer.h"

namespace iidxtra::settings
{
    static database::db* storage = nullptr;

    template <typename Action>
    static auto for_each_timing_setting(Action& apply, const std::string& prefix,
                                       const std::string& suffix, bm2dx::timing_t& timing) -> void
    {
        constexpr auto limit = std::numeric_limits<float>::max();
        apply(prefix + ".early_poor" + suffix, timing.early_poor, -limit, limit);
        apply(prefix + ".early_bad" + suffix, timing.early_bad, -limit, limit);
        apply(prefix + ".early_good" + suffix, timing.early_good, -limit, limit);
        apply(prefix + ".early_great" + suffix, timing.early_great, -limit, limit);
        apply(prefix + ".early_pgreat" + suffix, timing.early_pgreat, -limit, limit);
        apply(prefix + ".late_pgreat" + suffix, timing.late_pgreat, -limit, limit);
        apply(prefix + ".late_great" + suffix, timing.late_great, -limit, limit);
        apply(prefix + ".late_good" + suffix, timing.late_good, -limit, limit);
        apply(prefix + ".late_bad" + suffix, timing.late_bad, -limit, limit);
    }

    template <typename Action>
    static auto for_each_setting(Action apply) -> void
    {
        apply("interface.menu_lock", gui::play_lock_state);
        apply("hi_speed_reset.enabled", hi_speed_reset::enabled);

        #if FORCE_EVENT_MODE_ENABLED == 0
        apply("settings.event_mode", gui::config_event_mode);
        #endif

        apply("regular_speed.enabled", regular_speed::enabled);
        apply("chart_speed.rate", chart_speed::rate, chart_speed::rate_min, chart_speed::rate_max);
        apply("chart_speed.previous", chart_speed::rate_previous, chart_speed::rate_min, chart_speed::rate_max);
        apply("chart_speed.pitch", chart_speed::pitch_follows_rate);

        apply("keysound.mode", keysound_switch::override_type, 0, 3);
        apply("audio.global_percent", audio_balance::global_percent, 0, audio_balance::global_max_percent);
        apply("audio.keysound_percent", audio_balance::keysound_percent, 0, audio_balance::max_percent);
        apply("audio.bgm_percent", audio_balance::bgm_percent, 0, audio_balance::max_percent);
        apply("audio.custom_select", custom_audio::select_file);
        apply("audio.custom_decide", custom_audio::decide_file);
        apply("audio.custom_result", custom_audio::result_file);
        apply("gauge.tint_easy", gauge::tint_easy);
        apply("note_colors.p1.apply_to_beams", note_colors::apply_to_beams[0]);
        apply("note_colors.p2.apply_to_beams", note_colors::apply_to_beams[1]);
        for (std::size_t player = 0; player < note_colors::players.size(); ++player)
            for (std::size_t column = 0; column < note_colors::players[player].size(); ++column)
            {
                const auto prefix = "note_colors.p" + std::to_string(player + 1) +
                    (column == 7 ? ".scratch" : ".key" + std::to_string(column + 1));
                auto& color = note_colors::players[player][column];
                apply(prefix + ".tint", color.tint_enabled);
                apply(prefix + ".red", color.tint[0], 0, 1);
                apply(prefix + ".green", color.tint[1], 0, 1);
                apply(prefix + ".blue", color.tint[2], 0, 1);
                apply(prefix + ".saturation", color.saturation_percent, 0, 100);
            }

        apply("visuals.dark_mode", play_visuals::dark_mode);
        apply("visuals.io_key_display", play_visuals::io_key_display);
        apply("visuals.bga_darkness", play_visuals::bga_darkness, 0, 100);
        apply("visuals.no_measure_lines", play_visuals::no_measure_lines);
        apply("visuals.no_bpm_gradient", play_visuals::no_bpm_gradient);
        apply("visuals.concentration_movie", play_visuals::concentration_movie);
        apply("visuals.subscreen_dim_level", play_visuals::subscreen_dim_level, 0, 4);
        apply("visuals.scratch_flip.p1", scratch_flip::enabled_p1);
        apply("visuals.scratch_flip.p2", scratch_flip::enabled_p2);
        apply("fast_slow.milliseconds", fast_slow_display::options.show_milliseconds);
        apply("fast_slow.pgreat", fast_slow_display::options.show_pgreat);
        apply("timing_histogram.enabled", timing_histogram::enabled);
        apply("live_timing.mode", live_timing::display_mode, 0, 2);
        apply("live_timing.y_position", live_timing::y_position, 0, live_timing::max_y);
        apply("live_timing.flip_left_right", live_timing::flip_left_right);

        apply("autoretry.enabled", autoretry::enabled);
        apply("autoretry.target", autoretry::target, 0, 2);
        apply("autoretry.destination", autoretry::destination, 0, 1);

        apply("autoplay.p1", autoplay::enabled_p1);
        apply("autoplay.p2", autoplay::enabled_p2);

        apply("cn_override.p1", cn_override::p1_override_type, cn_override::ChartDefault, cn_override::LN);
        apply("cn_override.p2", cn_override::p2_override_type, cn_override::ChartDefault, cn_override::LN);
        apply("cn.enabled.p1", cn_transformer::enabled_p1);
        apply("cn.enabled.p2", cn_transformer::enabled_p2);
        apply("cn.percentage.p1", cn_transformer::long_note_percentage_p1, 5, 100);
        apply("cn.percentage.p2", cn_transformer::long_note_percentage_p2, 5, 100);
        apply("cn.duration.p1", cn_transformer::max_cn_duration_p1, -1, 10000);
        apply("cn.duration.p2", cn_transformer::max_cn_duration_p2, -1, 10000);
        apply("cn.scratch.p1", cn_transformer::allow_backspin_scratch_p1);
        apply("cn.scratch.p2", cn_transformer::allow_backspin_scratch_p2);

        apply("random.enabled.p1", unrandomizer::enabled_p1);
        apply("random.enabled.p2", unrandomizer::enabled_p2);

        #if BLOCK_FORCE_RANDOM_SAVE == 0
        apply("random.save_scores.p1", unrandomizer::allow_score_save_p1);
        apply("random.save_scores.p2", unrandomizer::allow_score_save_p2);
        #endif

        apply("random.show_info.p1", unrandomizer::show_random_info_p1);
        apply("random.show_info.p2", unrandomizer::show_random_info_p2);
        apply("random.hran.p1", unrandomizer::hran_enabled_p1);
        apply("random.hran.p2", unrandomizer::hran_enabled_p2);
        apply("scratch_swap.enabled.p1", scratch_swap::enabled_p1);
        apply("scratch_swap.enabled.p2", scratch_swap::enabled_p2);
        apply("scratch_swap.lane.p1", scratch_swap::swap_lane_p1, -1, 6);
        apply("scratch_swap.lane.p2", scratch_swap::swap_lane_p2, -1, 6);

        for (std::size_t lane = 0; lane < unrandomizer::column_lut_p1.size(); ++lane)
        {
            apply("random.lane" + std::to_string(lane) + ".p1", unrandomizer::column_lut_p1[lane], 0, 6);
            apply("random.lane" + std::to_string(lane) + ".p2", unrandomizer::column_lut_p2[lane], 0, 6);
        }

        apply("timing.keys.p1", timing_modifier::enabled_keys[0]);
        apply("timing.keys.p2", timing_modifier::enabled_keys[1]);
        apply("timing.scratch.p1", timing_modifier::enabled_scratch[0]);
        apply("timing.scratch.p2", timing_modifier::enabled_scratch[1]);
        for_each_timing_setting(apply, "timing.keys", ".p1", timing_modifier::override[0].keys);
        for_each_timing_setting(apply, "timing.keys", ".p2", timing_modifier::override[1].keys);
        for_each_timing_setting(apply, "timing.scratch", ".p1", timing_modifier::override[0].scratch);
        for_each_timing_setting(apply, "timing.scratch", ".p2", timing_modifier::override[1].scratch);
    }

    static auto is_valid_setting_value(double value, double minimum, double maximum) -> bool
    {
        if (!std::isfinite(value))
            return false;

        if (value < minimum || value > maximum)
            return false;

        return true;
    }

    template <typename Setting>
    static auto load_setting_value(Setting& setting, const database::setting_value& saved,
                                   double minimum, double maximum) -> void
    {
        if constexpr (std::is_same_v<Setting, std::string>)
        {
            if (auto const* value = std::get_if<std::string>(&saved))
                setting = *value;
        }
        else if constexpr (std::is_floating_point_v<Setting>)
        {
            auto const* value = std::get_if<double>(&saved);
            if (value != nullptr && is_valid_setting_value(*value, minimum, maximum))
                setting = static_cast<Setting>(*value);
        }
        else
        {
            auto const* value = std::get_if<std::int64_t>(&saved);
            if (value != nullptr && is_valid_setting_value(static_cast<double>(*value), minimum, maximum))
                setting = static_cast<Setting>(*value);
        }
    }

    template <typename Setting>
    static auto save_setting_value(Setting setting) -> database::setting_value
    {
        if constexpr (std::is_same_v<Setting, std::string>)
            return setting;
        else if constexpr (std::is_floating_point_v<Setting>)
            return static_cast<double>(setting);
        else
            return static_cast<std::int64_t>(setting);
    }

    auto init(database::db* db) -> void
    {
        storage = db;

        // Load settings from the database
        auto const values = database::load_settings(storage);
        if (values.empty())
        {
            audio_balance::update();
            custom_audio::update();
            gauge::update_tint();
            note_colors::update();
            live_timing::update();
            play_visuals::update_bga_darkness();
            play_visuals::update_io_key_display();
            return;
        }

        // Initialize settings from values loaded from the database
        for_each_setting([&](const std::string& key, auto& setting, double min = 0, double max = 1)
        {
            for (auto const& [saved_key, saved_value] : values)
            {
                if (saved_key != key)
                    continue;

                // Depending on the type of the setting, assign the value if it is valid
                load_setting_value(setting, saved_value, min, max);
                break;
            }
        });

        // Reset unrandomizer if the loaded state is invalid
        if (!unrandomizer::is_valid(0) || !unrandomizer::is_valid(1))
            unrandomizer::reset();

        // Update runtime settings
        audio_balance::update();
        custom_audio::update();
        gauge::update_tint();
        note_colors::update();
        play_visuals::update_dark_mode();
        play_visuals::update_bga_darkness();
        play_visuals::update_io_key_display();
        play_visuals::update_no_measure_lines();
        play_visuals::update_no_bpm_gradient();
        play_visuals::update_concentration_movie();
        play_visuals::update_subscreen_dim();
        scratch_flip::update();
        fast_slow_display::update();
        timing_histogram::update();
        live_timing::update();
    }

    auto save() -> bool
    {
        database::settings_t values;
        for_each_setting([&](const std::string& key, const auto& setting, double = 0, double = 1)
        {
            values.emplace_back(key, save_setting_value(setting));
        });
        return database::save_settings(storage, values);
    }

    auto reset() -> void
    {

        #if FORCE_EVENT_MODE_ENABLED
        gui::config_event_mode = true;
        #else 
        gui::config_event_mode = false;
        #endif

        reset_state_hook::reset_options();
    }
}