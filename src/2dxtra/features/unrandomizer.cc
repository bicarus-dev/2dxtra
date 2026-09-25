#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <ranges>
#include <string_view>
#include <MinHook.h>
#include <safetyhook.hpp>
#ifdef _MSC_VER
#include <intrin.h>
#endif
#include "unrandomizer.h"
#include "../log.h"
#include "../hooks/score_invalidator_hook.h"

namespace iidxtra::unrandomizer
{
	// options
    auto enabled_p1 = false;
    auto enabled_p2 = false;

    auto allow_score_save_p1 = false;
    auto allow_score_save_p2 = false;

	auto show_random_info_p1 = false;
	auto show_random_info_p2 = false;

	auto hran_enabled_p1 = false;
	auto hran_enabled_p2 = false;

	struct random_history
	{
		std::byte unused[0xE8];
		std::int32_t last_group[2];
		std::int32_t previous_group[2];
	};
	static_assert(offsetof(random_history, last_group) == 0xE8);
	static_assert(offsetof(random_history, previous_group) == 0xF0);
	static_assert(sizeof(random_history) == 0xF8);

	static auto hran_hook = SafetyHookMid {};
	static auto hran_label_hooks = std::array<SafetyHookInline, 3> {};

	auto hran_available() -> bool
		{ return static_cast<bool>(hran_hook); }

	// Native release string layout. Only replace characters; never allocate or free across modules.
	struct option_text
	{
		union
		{
			char local[16];
			char* heap;
		};
		std::size_t length;
		std::size_t capacity;
	};
	static_assert(sizeof(option_text) == 0x20);
	static_assert(offsetof(option_text, length) == 0x10);
	static_assert(offsetof(option_text, capacity) == 0x18);

	static auto update_hran_labels(option_text* text, unsigned int player) -> void
	{
		if (!hran_available() || (!hran_enabled_p1 && !hran_enabled_p2))
			return;

		if (text == nullptr || text->length > text->capacity ||
			(text->capacity >= sizeof(text->local) && text->heap == nullptr))
		{
			log::print("[Un-randomizer] Invalid native option text");
			return;
		}

		auto* const buffer = text->capacity < sizeof(text->local) ? text->local : text->heap;
		auto const formatted_options = std::string_view { buffer, text->length };
		auto const sp_hran_enabled =
			(player == 0 && hran_enabled_p1) || (player == 1 && hran_enabled_p2);

		for (auto option_range: formatted_options | std::views::split(','))
		{
			auto option = std::string_view { option_range };
			while (!option.empty() && option.front() == ' ')
				option.remove_prefix(1);

			auto const offset = option.data() - buffer;
			auto const side_separator = option.find('/');
			if (side_separator == std::string_view::npos)
			{
				if (sp_hran_enabled && option == "S-RANDOM")
					buffer[offset] = 'H';
				continue;
			}

			if (hran_enabled_p1 && option.substr(0, side_separator) == "S-RAN")
				buffer[offset] = 'H';
			if (hran_enabled_p2 && option.substr(side_separator + 1) == "S-RAN")
				buffer[offset + side_separator + 1] = 'H';
		}
	}

	template <std::size_t index>
	static auto option_label_hook_fn(void* options, option_text* text, unsigned int player,
		unsigned int style, char spaced) -> option_text*
	{
		auto* const formatted_text =
			hran_label_hooks[index].call<option_text*>(options, text, player, style, spaced);
		update_hran_labels(formatted_text, player);
		return formatted_text;
	}

	static auto pacemaker_label_hook_fn(option_text* text) -> option_text*
	{
		auto* const formatted_text = hran_label_hooks[2].call<option_text*>(text);
		auto const player = (!bm2dx::state->p1_active && bm2dx::state->p2_active) ? 1u : 0u;
		update_hran_labels(formatted_text, player);
		return formatted_text;
	}

	static auto install_hran_label_hooks() -> void
	{
		auto const targets = std::array {
			bm2dx::addr->RANDOM_OPTION_TEXT_FN,
			bm2dx::addr->RANDOM_RESULT_TEXT_FN,
			bm2dx::addr->RANDOM_PACEMAKER_TEXT_FN };
		if (std::ranges::any_of(targets, [] (auto* target) { return target == nullptr; }))
		{
			log::print("[Un-randomizer] H-RAN labels unavailable for this game version");
			return;
		}

		// Only live-option formatters; rival and saved-score text remains untouched.
		hran_label_hooks[0] = safetyhook::create_inline(targets[0], option_label_hook_fn<0>);
		hran_label_hooks[1] = safetyhook::create_inline(targets[1], option_label_hook_fn<1>);
		hran_label_hooks[2] = safetyhook::create_inline(targets[2], pacemaker_label_hook_fn);

		for (auto i = std::size_t { 0 }; i < hran_label_hooks.size(); ++i)
		{
			if (hran_label_hooks[i])
				continue;

			log::print("[Un-randomizer] Failed to install H-RAN label hook {}", i);
			for (auto& hook: hran_label_hooks)
				hook.reset();
			return;
		}
	}

	static auto hran_candidate_hook_fn(SafetyHookContext& ctx) -> void
	{
		if (!((ctx.r15 == 0 && hran_enabled_p1) || (ctx.r15 == 1 && hran_enabled_p2)))
			return;

		auto const player = static_cast<std::uint8_t>(ctx.r15);
		auto const* history = reinterpret_cast<const random_history*>(ctx.rdi);
		auto const now = static_cast<std::int32_t>(ctx.r14);
		auto const last_end = static_cast<std::int32_t>(ctx.rbx);

		// Before the native signed gap comparison: EAX is the minimum gap, ECX the elapsed gap.
		// Reject preceding-group lanes without changing the RNG, holds, or exhaustion fallback.
		if (last_end == history->last_group[player] ||
			(history->last_group[player] == now && last_end == history->previous_group[player]))
			ctx.rcx = static_cast<std::uint32_t>(ctx.rax) - 1u;

		if (bm2dx::state->play_style == 1)
		{
			score_invalidator_hook::invalidate(0);
			score_invalidator_hook::invalidate(1);
		}
		else
			score_invalidator_hook::invalidate(player);
	}

	// state
	auto column_lut_p1 = random_lut_t { 0, 1, 2, 3, 4, 5, 6 };
    auto column_lut_p2 = random_lut_t { 0, 1, 2, 3, 4, 5, 6 };

    // original functions
    void* original_game_apply_random_fn = nullptr;

	auto is_valid(const std::uint8_t player) -> bool
	{
		auto seen = std::array<bool, lane_count> {};

		for (auto const i: player == 0 ? column_lut_p1: column_lut_p2)
		{
			if (i >= lane_count)
				return false;

			seen[i] = true;
		}

		return std::ranges::all_of(seen, [] (auto v) { return v; });
	}

    auto reset() -> void
    {
	    auto constexpr lut_default = random_lut_t { 0, 1, 2, 3, 4, 5, 6 };
        enabled_p1 = false;
        enabled_p2 = false;
        allow_score_save_p1 = false;
        allow_score_save_p2 = false;
        show_random_info_p1 = false;
        show_random_info_p2 = false;
        hran_enabled_p1 = false;
        hran_enabled_p2 = false;
		std::ranges::copy(lut_default, column_lut_p1.begin());
		std::ranges::copy(lut_default, column_lut_p2.begin());
    }

	auto apply_force_random(std::uint8_t player) -> void
	{
		// pre-flight checks
		auto const enabled = (player == 0 ? enabled_p1: enabled_p2);
		auto const valid = (enabled && is_valid(player));

		if (!enabled || !valid)
			return;

		// convert textage compatible setting into value expected by the game
		auto const& lut = (player == 0 ? column_lut_p1 : column_lut_p2);

		for (auto i = std::size_t { 0 }; i < lane_count; i++)
		{
			auto const value = lut[i];
			bm2dx::random_data->columns[player][value] = static_cast<std::uint32_t>(i);
		}

        // if player is present, invalidate score
        if (player == 0 && bm2dx::state->p1_active && !allow_score_save_p1)
            score_invalidator_hook::invalidate(0);
        else if (player == 1 && bm2dx::state->p2_active && !allow_score_save_p2)
            score_invalidator_hook::invalidate(1);
	}

	auto game_random_to_string(const std::uint8_t player, random_lut_t& values) -> std::string
	{
		auto result = std::string {};

		for (auto i = std::size_t { 0 }; i < lane_count; i++)
		{
			auto const value = bm2dx::random_data->columns[player][i];

			if (value >= lane_count)
				continue;

			values[value] = static_cast<std::uint8_t>(i);
		}

		for (auto const column: values)
			result.append(std::to_string(column + 1));

		return result;
	}

	auto print_random_info(const std::uint8_t player) -> void
	{
		// get current random as string
		auto translated_random = random_lut_t {};
		auto current = game_random_to_string(player, translated_random);

		// don't display 1234567 (non-ran, s-ran)
		if (current != "1234567")
		{
			auto type = std::string { "?" };

			if (bm2dx::state->play_style == 1)
			{
				type = (player == 0 ? "LEFT": "RIGHT");
				log::print("[Un-randomizer] RANDOM: {} ({})", current, type);
			}
			else
			{
				if (player == 0 && bm2dx::state->p1_active && (show_random_info_p1 || enabled_p1))
					log::print("[Un-randomizer] P1 RANDOM: {}{}", current, (enabled_p1 ? " (CUSTOM)": ""));
				else if (player == 1 && bm2dx::state->p2_active && (show_random_info_p2 || enabled_p2))
					log::print("[Un-randomizer] P2 RANDOM: {}{}", current, (enabled_p2 ? " (CUSTOM)": ""));
			}
		}

		// update lut value to use the random just generated by the game
		// (makes playing or keeping track of good randoms easier)
		if (player == 0 && !enabled_p1)
			std::ranges::copy(translated_random, column_lut_p1.begin());

		if (player == 1 && !enabled_p2)
			std::ranges::copy(translated_random, column_lut_p2.begin());
	}

	struct random_sequence_data
	{
		std::byte unused[0x58];
		std::int32_t status[2];
		std::int32_t sequence[2];
	};
	static_assert(offsetof(random_sequence_data, status) == 0x58);
	static_assert(offsetof(random_sequence_data, sequence) == 0x60);

	static auto original_random_ticket_data_fn = static_cast<random_sequence_data*(*)()>(nullptr);

	static auto random_ticket_data_hook_fn() -> random_sequence_data*
	{
		// Call the original first.
		auto* native = original_random_ticket_data_fn();
		auto const show_sequence_p1 = show_random_info_p1 || enabled_p1;
		auto const show_sequence_p2 = show_random_info_p2 || enabled_p2;

		// Feature is disabled.
		if (!show_sequence_p1 && !show_sequence_p2)
			return native;
		
		// Random data cannot be retrieved.
		if (native == nullptr ||
			bm2dx::state == nullptr ||
			bm2dx::random_data == nullptr)
			return native;

		// Check if the caller is the Random Sequence renderer.
#ifdef _MSC_VER
		auto const caller = _ReturnAddress();
#else
		auto const caller = __builtin_return_address(0);
#endif
		DWORD64 image_base = 0;
		auto const function = RtlLookupFunctionEntry(reinterpret_cast<DWORD64>(caller), &image_base, nullptr);
		if (function == nullptr || image_base + function->BeginAddress !=
			reinterpret_cast<DWORD64>(bm2dx::addr->RANDOM_SEQUENCE_DRAW_FN))
			return native;

		// A pointer to the sequence is returned, so static storage is required.
		static thread_local auto display = random_sequence_data {};
		display = *native;

		for (std::uint8_t player = 0; player < 2; ++player)
		{
			// Check if DP; if so, either option activates it.
			auto const active =
				bm2dx::state->play_style != 0 ||
				(player == 0 ? show_sequence_p1 && bm2dx::state->p1_active :
					show_sequence_p2 && bm2dx::state->p2_active);

			if (!active)
				continue;

			// Get random data for this player / side.
			auto lanes = random_lut_t {};
			if (game_random_to_string(player, lanes) == "1234567")
			{
				display.status[player] = 0;
				continue;
			}

			// The native renderer expects a success flag and a seven-digit number.
			display.status[player] = 1;
			display.sequence[player] = 0;
			for (auto const column: lanes)
				display.sequence[player] = display.sequence[player] * 10 + column + 1;
		}

		return &display;
	}

	void install_hook()
	{
		MH_CreateHook(bm2dx::addr->APPLY_RANDOM_FN, (void*) +[] (void* a1) -> char
		{
			// apply game random immediately
			auto const result = reinterpret_cast<char (*) (void*)>
				(original_game_apply_random_fn) (a1);

			apply_force_random(0);
			apply_force_random(1);

			print_random_info(0);
			print_random_info(1);

			return result;
		}, &original_game_apply_random_fn);

		if (bm2dx::addr->RANDOM_SEQUENCE_DRAW_FN != nullptr &&
			bm2dx::addr->RANDOM_TICKET_DATA_FN != nullptr)
			MH_CreateHook(bm2dx::addr->RANDOM_TICKET_DATA_FN,
				reinterpret_cast<LPVOID>(random_ticket_data_hook_fn),
				reinterpret_cast<LPVOID*>(&original_random_ticket_data_fn));

		auto* const candidate_check = bm2dx::addr->SRAN_CANDIDATE_CHECK;
		if (candidate_check == nullptr)
		{
			log::print("[Un-randomizer] H-RAN is unavailable for this game version");
			return;
		}

		auto constexpr expected = std::array<std::uint8_t, 6> { 0x3B, 0xC8, 0x7C, 0xB6, 0xEB, 0x35 };
		if (std::memcmp(candidate_check, expected.data(), expected.size()) != 0)
		{
			log::print("[Un-randomizer] H-RAN is unavailable: S-RAN candidate check does not match");
			return;
		}

		hran_hook = safetyhook::create_mid(candidate_check, hran_candidate_hook_fn);
		if (!hran_hook)
		{
			log::print("[Un-randomizer] Failed to install H-RAN hook");
			return;
		}

		install_hran_label_hooks();
	}
}