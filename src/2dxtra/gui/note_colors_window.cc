#include "gui.h"
#include "note_colors_window.h"
#include "../features/note_colors.h"

namespace iidxtra::gui::note_colors_window
{
    bool visible = false;

    static auto draw_player_tab(int player) -> void
    {
        if (!ImGui::BeginTabItem(player == 0 ? "P1/LEFT" : "P2/RIGHT"))
            return;

        ImGui::PushID(player);
        ImGui::TextDisabled("Tint replaces the color; original texture shading and opacity are preserved.");
        ImGui::BeginDisabled(!note_colors::available());
        if (ImGui::BeginTable("Columns", 4, ImGuiTableFlags_SizingFixedFit))
        {
            ImGui::TableSetupColumn("Column", ImGuiTableColumnFlags_WidthFixed, 65);
            ImGui::TableSetupColumn("Tint", ImGuiTableColumnFlags_WidthFixed, 155);
            ImGui::TableSetupColumn("Saturation", ImGuiTableColumnFlags_WidthFixed, 160);
            ImGui::TableSetupColumn("Reset", ImGuiTableColumnFlags_WidthFixed, 55);
            ImGui::TableHeadersRow();

            for (int row = 0; row < 8; ++row)
            {
                const int column = row == 0 ? 7 : row - 1;
                auto& options = note_colors::players[player][column];
                ImGui::PushID(column);
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                if (column == 7)
                    ImGui::TextUnformatted("Scratch");
                else
                    ImGui::Text("Note %d", column + 1);

                ImGui::TableNextColumn();
                if (ImGui::Checkbox("##TintEnabled", &options.tint_enabled))
                    note_colors::update();
                ImGui::SameLine();
                ImGui::BeginDisabled(!options.tint_enabled);
                if (ImGui::ColorEdit3("##Tint", options.tint.data(),
                    ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoOptions))
                    note_colors::update();
                ImGui::EndDisabled();

                ImGui::TableNextColumn();
                ImGui::SetNextItemWidth(150);
                if (ImGui::SliderInt("##Saturation", &options.saturation_percent, 0, 100, "%d%%"))
                    note_colors::update();

                ImGui::TableNextColumn();
                if (ImGui::Button("Reset"))
                {
                    options = {};
                    note_colors::update();
                }

                ImGui::PopID();
            }
            ImGui::EndTable();
        }

        if (ImGui::Button("Reset this side"))
        {
            note_colors::players[player] = {};
            note_colors::update();
        }
        ImGui::SameLine();
        if (ImGui::Button("Copy to other player"))
            note_colors::copy_to_other_player(player);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Mirrors keys 1-7; scratch copies to scratch.");
        ImGui::EndDisabled();
        ImGui::PopID();
        ImGui::EndTabItem();
    }

    auto render() -> void
    {
        ImGui::SetNextWindowPos({ImGui::GetIO().DisplaySize.x * 0.5f, ImGui::GetIO().DisplaySize.y * 0.5f},
            0, {0.5f, 0.5f});
        ImGui::SetNextWindowSize({570, 420});
        ImGui::Begin("Configuration :: Note Colors", nullptr, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize);

        if (ImGui::IsWindowAppearing())
            ImGui::SetWindowFocus();

        if (ImGui::BeginTabBar("NoteColors"))
        {
            draw_player_tab(0);
            draw_player_tab(1);
            ImGui::EndTabBar();
        }

        const auto message = note_colors::status();
        if (!message.empty())
            ImGui::TextWrapped("%s", message.c_str());

        if (ImGui::Button("Return to main menu"))
            visible = false;
        ImGui::End();
    }
}
