#include <algorithm>
#include <string>
#include <imgui.h>
#include "customize_tab.h"
#include "../features/custom_audio.h"
#include "../features/result_backgrounds.h"

namespace iidxtra::gui::customize_tab
{
    static auto custom_audio_combo(const char* label, custom_audio::channel channel,
                                  std::string& selection) -> void
    {
        auto const& files = custom_audio::files(channel);
        auto const can_follow_bgm = channel == custom_audio::channel::music_decide;
        auto const follows_bgm = can_follow_bgm && selection == custom_audio::follow_bgm;
        auto const missing = !selection.empty() && selection != "*" && !follows_bgm &&
            std::find(files.begin(), files.end(), selection) == files.end();
        auto const preview = selection.empty() ? std::string("Default") :
            selection == "*" ? std::string("Random") :
            follows_bgm ? std::string("Follow BGM") :
            (missing ? "(unavailable) " : "") + selection;

        ImGui::PushID(label);
        ImGui::TextUnformatted(label);
        ImGui::SameLine(285);
        ImGui::SetNextItemWidth(240);
        if (ImGui::BeginCombo("##CustomAudio", preview.c_str()))
        {
            auto option = [&](const char* text, const std::string& value)
            {
                auto const selected = selection == value;
                if (ImGui::Selectable(text, selected) && !selected)
                {
                    selection = value;
                    custom_audio::update();
                }
                if (selected)
                    ImGui::SetItemDefaultFocus();
            };
            option("Default", "");
            option("Random", "*");
            if (can_follow_bgm)
                option("Follow BGM", custom_audio::follow_bgm);
            if (missing)
                ImGui::Selectable(preview.c_str(), true, ImGuiSelectableFlags_Disabled);
            for (auto const& file : files)
                option(file.c_str(), file);
            ImGui::EndCombo();
        }
        ImGui::PopID();
    }

    static auto result_background_combo(const char* label, result_backgrounds::pool kind) -> void
    {
        auto& selection = result_backgrounds::selections[static_cast<std::size_t>(kind)];
        auto const missing = !selection.empty() && selection != "*" &&
            !result_backgrounds::has_subfolder(kind, selection);
        auto const preview = selection.empty() ? std::string("Default") :
            selection == "*" ? std::string("Random") : (missing ? "(unavailable) " : "") + selection;
        ImGui::PushID(label);
        ImGui::TextUnformatted(label);
        ImGui::SameLine(285);
        ImGui::SetNextItemWidth(240);
        if (ImGui::BeginCombo("##ResultBackground", preview.c_str()))
        {
            auto option = [&](const char* text, const std::string& value)
            {
                auto const selected = selection == value;
                if (ImGui::Selectable(text, selected) && !selected)
                {
                    selection = value;
                    result_backgrounds::update();
                }
                if (selected)
                    ImGui::SetItemDefaultFocus();
            };
            option("Default", "");
            option("Random", "*");
            if (missing)
                ImGui::Selectable(preview.c_str(), true, ImGuiSelectableFlags_Disabled);
            for (auto const& subfolder : result_backgrounds::subfolders(kind))
                option(subfolder.c_str(), subfolder);
            ImGui::EndCombo();
        }
        ImGui::PopID();
    }

    auto render() -> void
    {
        if (ImGui::CollapsingHeader("Menu music", ImGuiTreeNodeFlags_DefaultOpen))
        {
            ImGui::BeginDisabled(!custom_audio::available());
            custom_audio_combo("Music select", custom_audio::channel::music_select,
                               custom_audio::select_file);
            custom_audio_combo("Music decide", custom_audio::channel::music_decide,
                               custom_audio::decide_file);
            custom_audio_combo("Result BGM", custom_audio::channel::result,
                               custom_audio::result_file);
            ImGui::EndDisabled();
            auto const status = custom_audio::status();
            if (!status.empty())
                ImGui::TextWrapped("%s", status.c_str());
        }

        if (ImGui::CollapsingHeader("Result Backgrounds", ImGuiTreeNodeFlags_DefaultOpen))
        {
            ImGui::BeginDisabled(!result_backgrounds::available());
            result_background_combo("A", result_backgrounds::pool::a);
            result_background_combo("AA", result_backgrounds::pool::aa);
            result_background_combo("AAA", result_backgrounds::pool::aaa);
            result_background_combo("Pass", result_backgrounds::pool::pass);
            result_background_combo("Fail", result_backgrounds::pool::fail);
            ImGui::EndDisabled();
            auto const status = result_backgrounds::status();
            if (!status.empty())
                ImGui::TextWrapped("%s", status.c_str());
        }
    }
}
