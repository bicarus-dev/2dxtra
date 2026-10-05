#include <algorithm>
#include <vector>
#include <imgui.h>
#include "../chart_set.h"
#include "loader.h"

namespace iidxtra::gui
{
    auto render_loader() -> void
    {
		auto const flags = chart_set::switch_enabled ? ImGuiSelectableFlags_None: ImGuiSelectableFlags_Disabled;

		if (!ImGui::CollapsingHeader("Loader", ImGuiTreeNodeFlags_DefaultOpen))
			return;

    	if (chart_set::switch_enabled)
    		ImGui::Text("Available chart sets (%zu):", chart_set::custom.size() + 1);
    	else
    		ImGui::TextColored({0.5f, 0.2f, 0.2f, 1.f}, "The active chart set can only be changed in music select");

		ImGui::Indent(10);
			if (ImGui::Selectable("Default", chart_set::active.empty(), flags))
				chart_set::revert();

			std::vector<const decltype(chart_set::custom)::value_type*> sorted_sets;
			sorted_sets.reserve(chart_set::custom.size());
			for (auto const& entry : chart_set::custom)
				sorted_sets.push_back(&entry);

			std::sort(sorted_sets.begin(), sorted_sets.end(), [](const auto* left, const auto* right)
			{
				if (left->second.id != right->second.id)
					return left->second.id < right->second.id;
				return left->first < right->first;
			});

			for (auto const* entry : sorted_sets)
			{
				auto const& [name, set] = *entry;
				if (ImGui::Selectable(name.c_str(), chart_set::active == name, flags))
					chart_set::set_active(name);

				ImGui::SameLine(285); ImGui::Text("%llu charts", set.count);
			}
		ImGui::Unindent(10);
    }
}