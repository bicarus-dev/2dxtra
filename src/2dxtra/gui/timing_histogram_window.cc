#include <algorithm>
#include <bitset>
#include <fmt/format.h>
#include <numeric>
#include <string>
#include "gui.h"
#include "timing_histogram_window.h"
#include "../features/timing_histogram.h"
#include "../input.h"

namespace iidxtra::gui::timing_histogram_window
{
    struct judgment_style_t
    {
        const char* label;
        ImU32 color;
    };

    constexpr auto judgment_styles =
        std::array<judgment_style_t, timing_histogram::judgment_count> {
        // white
        judgment_style_t {"PGREAT", IM_COL32(255, 255, 255, 255)},

        // cyan
        judgment_style_t {"GREAT", IM_COL32(0, 255, 255, 255)},

        // yellow
        judgment_style_t {"GOOD", IM_COL32(255, 255, 0, 255)}
    };

    auto draw_legend() -> void
    {
        auto* draw = ImGui::GetWindowDrawList();
        for (std::size_t category = 0; category < timing_histogram::judgment_count; ++category)
        {
            if (category != 0)
                ImGui::SameLine(0.0f, 6.0f);

            const auto position = ImGui::GetCursorScreenPos();
            const auto& style = judgment_styles[category];

            // color square
            draw->AddRectFilled(
                {position.x, position.y + 3.0f},
                {position.x + 10.0f, position.y + 13.0f},
                style.color);

            // spacing
            ImGui::Dummy({10.0f, ImGui::GetTextLineHeight()});
            ImGui::SameLine(0.0f, 3.0f);

            // label for legend
            ImGui::TextUnformatted(style.label);
        }

        ImGui::SameLine(0.0f, 3.0f);
        ImGui::TextDisabled("(excludes BAD & POOR)");

        ImGui::TextDisabled(
            "Each bar is %.2f ms; excludes CN release timing.",
            timing_histogram::bin_width_ms);
    }

    auto draw_summary(const char* label,
                      const timing_histogram::timing_tracker_t& distribution) -> void
    {
        const auto summary = distribution.count == 0 ?
            fmt::format("{}: {} hits | avg -- | sd -- ms", label, distribution.count) :
            fmt::format("{}: {} hits | avg {:+.1f} ms | sd {:.1f} ms",
                        label, distribution.count, distribution.mean(),
                        distribution.standard_deviation());

        ImGui::TextUnformatted(summary.c_str());
    }

    auto draw_compact_summary(
        const char* label, const timing_histogram::timing_tracker_t& distribution) -> float
    {
        ImGui::Text("%s: %u hits", label, distribution.count);
        const auto statistics = distribution.count == 0 ? std::string { "avg -- | sd -- " } :
            fmt::format("avg {:+.1f} | sd {:.1f}",
                        distribution.mean(), distribution.standard_deviation());
        const auto summary_top = ImGui::GetCursorPosY();
        ImGui::TextWrapped("%s", statistics.c_str());
        return ImGui::GetCursorPosY() - summary_top;
    }

    struct plot_layout_t
    {
        float left;
        float right;
        float top;
        float bottom;
        float height;
        float center;
        float pixels_per_ms;
        float column_width;
        std::uint32_t peak;
    };

    auto make_plot_layout(const timing_histogram::timing_tracker_t& distribution,
                          const float width, const float section_height,
                          const float extra_summary_height) -> plot_layout_t
    {
        const auto origin = ImGui::GetCursorScreenPos();
        const auto plot_height =
            section_height -
            extra_summary_height -
            2.0f * ImGui::GetTextLineHeight() -
            2.0f * ImGui::GetStyle().ItemSpacing.y -
            10.0f;

        const auto left = origin.x + 18.0f;
        const auto right = origin.x + width - 18.0f;
        const auto top = origin.y + 4.0f;
        const auto bottom = top + plot_height;

        const auto center = (left + right) * 0.5f;
        const auto pixels_per_ms = (right - left) / (2.0f * distribution.range_ms);
        const auto column_width = timing_histogram::bin_width_ms * pixels_per_ms;
        std::uint32_t peak = 1;
        for (const auto& bin : distribution.judgments)
        {
            const auto total = std::accumulate(bin.begin(), bin.end(), std::uint32_t { 0 });
            peak = std::max(peak, total);
        }

        return {left, right, top, bottom, plot_height, center, pixels_per_ms, column_width, peak};
    }

    auto draw_bars(const timing_histogram::timing_tracker_t& distribution,
                   const plot_layout_t& layout) -> void
    {
        auto* draw = ImGui::GetWindowDrawList();

        // Draw background
        draw->AddRectFilled({layout.left, layout.top}, {layout.right, layout.bottom},
                    IM_COL32(18, 18, 18, 220));

        // Stack the actual judgments within each timing bin; we do this since charge notes have
        // wider timing and can have different judgement even in the same timing window.
        for (int index = 0; index < timing_histogram::bin_count; ++index)
        {
            const auto milliseconds =
                static_cast<float>(index - timing_histogram::bin_count / 2) *
                timing_histogram::bin_width_ms;
            const auto horizontal = layout.center + milliseconds * layout.pixels_per_ms -
                layout.column_width * 0.5f;
            const auto bar_left = std::max(layout.left, horizontal + 0.5f);
            const auto bar_right = std::min(layout.right, horizontal + layout.column_width - 0.5f);

            std::uint32_t stack_height = 0;
            for (std::size_t category = 0; category < timing_histogram::judgment_count; ++category)
            {
                const auto segment_count = distribution.judgments[index][category];
                if (segment_count == 0)
                    continue;

                // Draw rect for this segment.
                const auto segment_bottom = layout.bottom -
                    layout.height * static_cast<float>(stack_height) /
                    static_cast<float>(layout.peak);

                stack_height += segment_count;

                const auto segment_top = layout.bottom -
                    layout.height * static_cast<float>(stack_height) /
                    static_cast<float>(layout.peak);
                draw->AddRectFilled({bar_left, segment_top},
                                    {bar_right, segment_bottom}, judgment_styles[category].color);
            }
        }

    }

    auto draw_axes(const timing_histogram::timing_tracker_t& distribution,
                   const plot_layout_t& layout, const bool compact) -> void
    {
        auto* draw = ImGui::GetWindowDrawList();

        // Draw bottom axis line.
        draw->AddLine({layout.left, layout.bottom}, {layout.right, layout.bottom},
                  IM_COL32(160, 160, 160, 255));

        // Draw bottom axis tick labels (two on left, two on right, and zero in the center)
        for (int tick = -2; tick <= 2; tick += compact ? 2 : 1)
        {
            const auto milliseconds = static_cast<float>(tick) * distribution.range_ms * 0.5f;
            const auto text = tick == 0 ? std::string { "0" } :
                fmt::format("{:+.0f}", milliseconds);
            const auto position = layout.center + milliseconds * layout.pixels_per_ms;
            draw->AddText({position - ImGui::CalcTextSize(text.c_str()).x * 0.5f,
                           layout.bottom + 3.0f},
                          IM_COL32(210, 210, 210, 255), text.c_str());
        }

        // Draw label for peak value to indicate y-axis scale.
        const auto peak_label = fmt::format("{}", layout.peak);
        draw->AddText(
            {layout.left + 3.0f, layout.top + 2.0f},
            IM_COL32(210, 210, 210, 255),
            peak_label.c_str());

    }

    auto draw_average_marker(const timing_histogram::timing_tracker_t& distribution,
                             const plot_layout_t& layout) -> void
    {
        auto* draw = ImGui::GetWindowDrawList();

        // Draw average marker colored by timing direction.
        if (distribution.count != 0)
        {
            const auto average = distribution.mean();
            const auto mean_position = layout.center +
                static_cast<float>(average) * layout.pixels_per_ms;

            // blue for early, white for 0.0, red for late
            const auto mean_color =
                average > 0.0 ?
                    IM_COL32(230, 50, 50, 255) :
                    average < 0.0 ?
                        IM_COL32(50, 150, 255, 255) :
                        IM_COL32(255, 255, 255, 255);

            if (mean_position >= layout.left && mean_position <= layout.right)
                draw->AddTriangleFilled({mean_position, layout.bottom - 1.0f},
                                        {mean_position + 6.0f, layout.bottom + 8.0f},
                                        {mean_position - 6.0f, layout.bottom + 8.0f}, mean_color);
        }
    }

    auto draw_distribution(const char* label,
                           const timing_histogram::timing_tracker_t& distribution,
                           const float section_height, const bool compact = false) -> void
    {
        const auto width = ImGui::GetContentRegionAvail().x;
        float extra_summary_height = 0.0f;
        if (compact)
            extra_summary_height = draw_compact_summary(label, distribution);
        else
            draw_summary(label, distribution);

        const auto layout =
            make_plot_layout(distribution, width, section_height, extra_summary_height);
        draw_bars(distribution, layout);
        draw_axes(distribution, layout, compact);
        draw_average_marker(distribution, layout);

        // Reserve space for the plot and the axis labels.
        ImGui::Dummy({width, layout.height + ImGui::GetTextLineHeight() + 10.0f});
    }

    auto render() -> void
    {
        static auto previous_starts = std::bitset<2> {};
        static auto player_views = std::array<int, 2> {};
        static int dp_view = 0;
        static std::uint64_t generation = 0;

        const auto result = timing_histogram::snapshot();
        if (!result.visible)
        {
            previous_starts.reset();
            return;
        }

        // Rising edge detection for start buttons, used to switch between views.
        const auto buttons = std::bitset<32>(bm2dx::input_manager->data.buttons_edge);
        auto starts = std::bitset<2> {};
        starts.set(0, buttons.test(static_cast<std::size_t>(bm2dx::button::P1_START)));
        starts.set(1, buttons.test(static_cast<std::size_t>(bm2dx::button::P2_START)));
        const auto pressed = starts & ~previous_starts;
        previous_starts = starts;

        if (gui::visible)
            return;

        if (generation != result.generation)
        {
            generation = result.generation;
            player_views = {};
            dp_view = 0;
        }
        else if (result.is_dp)
        {
            if (pressed.any())
                dp_view = (dp_view + 1) % 3;
        }
        else
        {
            for (std::size_t player = 0; player < player_views.size(); ++player)
                if (pressed.test(player))
                    player_views[player] = (player_views[player] + 1) % 2;
        }

        const auto display = ImGui::GetIO().DisplaySize;
        const auto width = display.x * (388.0f / 1920.0f);
        const auto height = display.y * (354.0f / 1080.0f);
        const auto left = display.x * (568.0f / 1920.0f);
        const auto top = display.y * (380.0f / 1080.0f);

        if (result.is_dp &&
            result.combined.keys.count == 0 &&
            result.combined.buttons[7].count == 0)
            return;

        const bool combined_view = result.is_dp && dp_view == 0;
        const auto player_count = combined_view ? 1 : 2;
        for (int player = 0; player < player_count; ++player)
        {
            const auto& data = combined_view ? result.combined : result.players[player];
            const bool button_view = result.is_dp ? dp_view == 2 : player_views[player] == 1;

            // Skip player if nothing to show on graph
            if (!result.is_dp && data.keys.count == 0 && data.buttons[7].count == 0)
                continue;

            // Window title
            auto title = std::string {};
            if (combined_view)
            {
                title = "DP timing histogram";
            }
            else if (result.is_dp)
            {
                title = fmt::format("DP ({}) {}",
                                    player ?
                                        "right" : "left",
                                    button_view ?
                                        "breakdown by button" : "timing histogram");
            }
            else
            {
                title = fmt::format("P{} {}", player + 1,
                                    button_view ?
                                        "breakdown by button" : "timing histogram");
            }

            title += " (Start to switch view)";

            // DP shows in center; otherwise, show on each side.
            const auto x_offset = combined_view ? (display.x - width) * 0.5f :
                (player == 0) ?
                left :
                display.x - left - width;

            ImGui::SetNextWindowPos({x_offset, top}, ImGuiCond_Always);
            ImGui::SetNextWindowSize(
                {width, button_view ? display.y * (600.0f / 1080.0f) : height});
            ImGui::SetNextWindowBgAlpha(0.92f);

            // No input, no move, no interactions.
            const auto flags =
                ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse |
                ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoInputs |
                ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                ImGuiWindowFlags_NoNav;

            if (ImGui::Begin(title.c_str(), nullptr, flags))
            {
                // header
                draw_legend();

                if (button_view)
                {
                    // Breakdown by each lane
                    const auto row_height = (ImGui::GetContentRegionAvail().y) * 0.25f -
                        2.0f * ImGui::GetStyle().CellPadding.y +
                        ImGui::GetStyle().ItemSpacing.y;

                    if (ImGui::BeginTable("ButtonHistograms", 2, ImGuiTableFlags_SizingStretchSame))
                    {
                        for (std::size_t lane = 0; lane < data.buttons.size(); ++lane)
                        {
                            ImGui::TableNextColumn();
                            const auto label =
                                lane == 7 ?
                                    std::string { "Scratch" } :
                                    fmt::format("Key {}", lane + 1);

                            draw_distribution(label.c_str(), data.buttons[lane], row_height, true);
                        }
                        ImGui::EndTable();
                    }
                }
                else
                {
                    // Combined view for keys and scratch
                    const auto separator_height = 1.0f + ImGui::GetStyle().ItemSpacing.y;
                    const auto graph_height = (ImGui::GetContentRegionAvail().y - separator_height) * 0.5f;
                    draw_distribution("Keys", data.keys, graph_height);
                    ImGui::Separator();
                    draw_distribution("Scratch", data.buttons[7], graph_height);
                }
            }
            ImGui::End();
        }
    }
}