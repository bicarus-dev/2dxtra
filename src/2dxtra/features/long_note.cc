#include <atomic>
#include <array>
#include <mutex>
#include <safetyhook.hpp>
#include "long_note.h"
#include "cn_override.h"
#include "note_colors.h"
#include "../game.h"
#include "../judgment.h"
#include "../hooks/fast_slow_hook.h"
#include "../log.h"

namespace iidxtra::long_note
{
    namespace
    {
        struct head_judgment
        {
            bm2dx::judge_candidate_t timing {};
            int grade = 0;
            int measure = -1;
        };
        SafetyHookMid held_hook, timing_hook, head_hook, count_hook;
        std::atomic_bool installed = false;
        std::atomic_uint enabled_players = 0;
        std::mutex head_mutex;
        std::array<std::array<head_judgment, 8>, 2> heads {};

        auto candidate_for(const void* context, std::uintptr_t player, std::uintptr_t lane)
            -> const bm2dx::judge_candidate_t*
        {
            if (!context || player >= 2 || lane >= 8 || !enabled(static_cast<int>(player)))
                return nullptr;

            const auto* result = static_cast<const bm2dx::judge_candidate_t*>(context) + player * 8 + lane;
            return result->note && result->note->is_charge_note() ? result : nullptr;
        }

        auto held_result(SafetyHookContext& context) -> void
        {
            const auto* note = candidate_for(reinterpret_cast<const void*>(context.r13), context.r15, context.rsi);
            if (!note)
                return;
            cn_override::invalidate_score(static_cast<std::uint8_t>(context.r15));
            // Candidate ticks are relative to the tail while a CN is being held.
            if (note->ticks >= 0)
                context.rax &= ~std::uintptr_t{0xff};
        }

        auto timing_result(SafetyHookContext& context) -> void
        {
            if (const auto* note = candidate_for(reinterpret_cast<const void*>(context.r13), context.r15, context.rsi))
            {
                const auto& timing = reinterpret_cast<const bm2dx::timing_data_t*>(context.r13)->timing[context.r15];
                const auto& windows = context.rsi == bm2dx::SCRATCH_COLUMN ? timing.scratch : timing.keys;
                // early_bad is the outer boundary of the native early-GOOD interval.
                const bool success = note->ticks >= 0 || note->milliseconds >= windows.early_bad;
                context.rax = success ? bm2dx::judge_grade::early_pgreat : bm2dx::judge_grade::early_bad;
            }
        }

        auto head_result(SafetyHookContext& context) -> void
        {
            const auto player = context.r14;
            const auto lane = context.rbp;
            const auto grade = static_cast<int>(context.rax);
            if (grade < bm2dx::judge_grade::early_bad || grade > bm2dx::judge_grade::late_bad)
                return;
            const auto* note = candidate_for(reinterpret_cast<const void*>(context.rdi), player, lane);
            if (!note || static_cast<std::uint8_t>(note->note->visible) == 0)
                return;
            const std::lock_guard lock(head_mutex);
            heads[player][lane] = {*note, grade, -1};
            // Native CNs cannot start on BAD. Arm the hold, but retain BAD as its final grade.
            if (grade == bm2dx::judge_grade::early_bad || grade == bm2dx::judge_grade::late_bad)
                context.rax = bm2dx::judge_grade::early_good;
        }

        auto count_note(SafetyHookContext& context) -> void
        {
            if (!available() || !context.r15 || context.r13 >= 2 || !(context.rsi & 0xffff))
                return;
            const auto event_type = *reinterpret_cast<const std::uint8_t*>(context.r15);
            const int player = bm2dx::state && bm2dx::state->play_style == 1 ?
                (event_type == 1 || event_type == 101 ? 1 : 0) : static_cast<int>(context.r13);
            if (!cn_override::active_player(player) ||
                (player == 0 ? cn_override::p1_override_type : cn_override::p2_override_type) != cn_override::LN)
                return;
            // Only the count pass's local CN length changes: totals, column and measure counts
            // see one note, while the chart's actual duration remains intact for gameplay.
            context.rsi = 0;
            cn_override::invalidate_score(static_cast<std::uint8_t>(player));
        }
    }

    auto available() -> bool
    {
        return installed.load() && fast_slow_hook::available() && note_colors::available();
    }

    auto enabled(int player) -> bool
    {
        return cn_override::active_player(player) && available() && (enabled_players.load() & (1u << player));
    }

    auto process_judgment(void* context, int player, int grade, int lane, int measure,
                         bool press, bool release) -> judgment
    {
        judgment result {false, grade, measure, {}};
        if (!press && !release)
            return result;
        const auto* candidate = candidate_for(context, player, lane);
        if (!candidate)
            return result;
        const std::lock_guard lock(head_mutex);
        auto& head = heads[player][lane];
        if (press && grade >= bm2dx::judge_grade::early_bad && grade <= bm2dx::judge_grade::late_bad)
        {
            if (head.timing.note != candidate->note)
            {
                log::print("[LN] Missing head classification; the hold will receive BAD.");
                head = {*candidate, bm2dx::judge_grade::early_bad, measure};
            }
            head.measure = measure;
            result.suppress = true;
        }
        else if (release)
        {
            if (head.timing.note != candidate->note)
            {
                log::print("[LN] Missing head judgment at release; awarding BAD.");
                result.grade = bm2dx::judge_grade::early_bad;
                return result;
            }
            result.grade = grade == bm2dx::judge_grade::early_pgreat ? head.grade :
                (head.timing.milliseconds < 0 ? bm2dx::judge_grade::early_bad : bm2dx::judge_grade::late_bad);
            result.measure = head.measure;
            result.head = head.timing;
            head = {};
        }
        return result;
    }

    auto set_enabled(std::uint8_t player, bool enabled) -> void
    {
        if (player >= 2)
        {
            log::print("[LN] Invalid player index: {}", player);
            return;
        }
        const unsigned bit = 1u << player;
        if (enabled && available())
            enabled_players.fetch_or(bit);
        else
        {
            enabled_players.fetch_and(~bit);
            const std::lock_guard lock(head_mutex);
            heads[player] = {};
        }
    }

    auto reset() -> void
    {
        enabled_players = 0;
        const std::lock_guard lock(head_mutex);
        heads = {};
    }

    auto shutdown() -> void
    {
        reset();
        installed = false;
        held_hook.reset();
        timing_hook.reset();
        head_hook.reset();
        count_hook.reset();
    }

    auto install_hook() -> void
    {
        const auto& addresses = *bm2dx::addr;
        if (!addresses.CN_RELEASE_INPUT_RETURN || !addresses.CN_RELEASE_TIMING_RETURN ||
            !addresses.CN_HEAD_TIMING_RETURN || !addresses.CHART_NOTE_COUNT_SETUP ||
            !addresses.CN_END_SPRITE_RETURN || !addresses.BSS_END_SPRITE_RETURN ||
            !fast_slow_hook::available() || !note_colors::available())
        {
            log::print("[LN] Long Note is unavailable for this game build.");
            return;
        }
        held_hook = safetyhook::create_mid(addresses.CN_RELEASE_INPUT_RETURN, held_result);
        timing_hook = safetyhook::create_mid(addresses.CN_RELEASE_TIMING_RETURN, timing_result);
        head_hook = safetyhook::create_mid(addresses.CN_HEAD_TIMING_RETURN, head_result);
        count_hook = safetyhook::create_mid(addresses.CHART_NOTE_COUNT_SETUP, count_note);
        if (!held_hook || !timing_hook || !head_hook || !count_hook)
        {
            shutdown();
            log::print("[LN] Could not install native release hooks.");
            return;
        }
        installed = true;
    }
}
