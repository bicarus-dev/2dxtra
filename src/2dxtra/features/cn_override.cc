#include <array>
#include <vector>
#include "../log.h"
#include "../game.h"
#include "../hooks/score_invalidator_hook.h"
#include "cn_override.h"
#include "long_note.h"

namespace iidxtra::cn_override
{
    namespace
    {
        struct original_note
        {
            bm2dx::play_note_t* note;
            int hcn;
        };
        struct chart_snapshot
        {
            const bm2dx::play_notes_t* source = nullptr;
            std::vector<original_note> charges;
        };
        std::array<chart_snapshot, 2> charts;
    }

    int p1_override_type = ChartDefault;
    int p2_override_type = ChartDefault;

    auto active_player(int player) -> bool
    {
        if (player < 0 || player >= 2 || !bm2dx::state || bm2dx::state->game_type == 9)
            return false;
        return bm2dx::state->play_style == 1 ?
            bm2dx::state->p1_active || bm2dx::state->p2_active :
            (player == 0 ? bm2dx::state->p1_active : bm2dx::state->p2_active);
    }

    auto invalidate_score(std::uint8_t player) -> void
    {
        if (player >= 2)
        {
            log::print("[CN] Invalid player index: {}", player);
            return;
        }
        if (!active_player(player))
            return;
        if (bm2dx::state->play_style == 1)
        {
            score_invalidator_hook::invalidate(0);
            score_invalidator_hook::invalidate(1);
        }
        else
            score_invalidator_hook::invalidate(player);
    }

    auto reset() -> void
    {
        p1_override_type = ChartDefault;
        p2_override_type = ChartDefault;
        charts = {};
        long_note::reset();
    }

    auto set_initial_states(const std::uint8_t player) -> void
    {
        if (player >= 2 || !bm2dx::play_field)
        {
            log::print("[CN] Cannot capture note types for player {} without a valid play field.", player);
            return;
        }
        long_note::set_enabled(player, false);
        auto& notes = bm2dx::play_field->notes[player];
        auto& chart = charts[player];
        chart.source = &notes;
        chart.charges.clear();
        for (auto& note : notes)
            if (note.is_charge_note())
                chart.charges.push_back({&note, note.hcn});
    }

    auto update(const std::uint8_t player) -> void
    {
        if (player >= 2)
        {
            log::print("[CN] Invalid player index: {}", player);
            return;
        }
        long_note::set_enabled(player, false);
        if (!bm2dx::play_field || !active_player(player))
            return;
        const int selection = player == 0 ? p1_override_type : p2_override_type;
        if (selection < ChartDefault || selection > LN || (selection == LN && !long_note::available()))
        {
            log::print("[CN] Requested charge note type {} is unavailable.", selection);
            return;
        }
        const auto& chart = charts[player];
        if (chart.source != &bm2dx::play_field->notes[player])
        {
            log::print("[CN] Original note types unavailable; override deferred until the next chart.");
            return;
        }
        bool has_cn = false;
        bool changed_from_chart = false;
        for (const auto& original : chart.charges)
        {
            auto& note = *original.note;
            if (!note.is_charge_note())
                continue;
            note.hcn = selection == ChartDefault ? original.hcn : (selection == HCN ? 1 : 0);
            changed_from_chart |= note.hcn != original.hcn;
            has_cn = true;
        }
        long_note::set_enabled(player, selection == LN && has_cn);

        if (has_cn && (selection == LN || changed_from_chart))
            invalidate_score(player);
    }
}