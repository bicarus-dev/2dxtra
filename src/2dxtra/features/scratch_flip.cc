#include <MinHook.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstring>
#include <string>
#include <type_traits>
#include "../game.h"
#include "../util/scoped_page_permissions.h"
#include "scratch_flip.h"

namespace iidxtra::scratch_flip
{
    auto enabled_p1 = false;
    auto enabled_p2 = false;

    struct point
    {
        std::int32_t x;
        std::int32_t y;
    };

    // Per-side X positions for lane covers, the red judge line, and judge/combo sprites.
    struct player_positions
    {
        std::array<std::int32_t, 2> players; // 1P X, then 2P X
    };
    struct play_field_positions
    {
        player_positions single_play;
        player_positions double_play;
    };

    // X coordinates for notes, beams and bomb sprites.
    struct lane_positions
    {
        std::array<std::int32_t, 7> keys;
        std::int32_t scratch;
    };
    struct single_play_lanes
    {
        std::array<lane_positions, 2> players;
    };

    // FAST/SLOW positions for one placement mode, with 1P/2P pairs for SP and DP.
    struct indicator_mode
    {
        std::array<point, 2> single_play;
        std::array<point, 2> double_play;
    };
    // All four native FAST/SLOW placement modes.
    struct indicator_positions
    {
        std::array<indicator_mode, 4> modes;
    };
    // FAST/SLOW positions for combined and separated modes, each with 4 position modes.
    struct indicator_position_block
    {
        indicator_positions combined;
        indicator_positions keys;
        indicator_positions scratch;
    };

    // Copy of the DLL's original position values.
    struct layout_positions
    {
        single_play_lanes notes;
        single_play_lanes beams;
        single_play_lanes effects;
        player_positions judgment;
        indicator_position_block indicators;
        play_field_positions field;
    };

    // Live state for the judge line (red line)
    // For this, the game reads in location values once, so its state must be monitored if we
    // want the option to change the red line's position.
    enum red_line_flags : std::uint32_t
    {
        positions_initialized = 1u << 0,
        height_initialized = 1u << 1
    };
    struct red_line_state
    {
        play_field_positions positions;
        red_line_flags initialization_flags;
    };

    // The native height value starts at byte 20 and must not be overwritten with this state.
    static_assert(sizeof(red_line_state) == 20);

    struct player_layers
    {
        void* play = nullptr;
        void* gauge = nullptr;
    };

    // Internal state.
    static auto native_positions = layout_positions {};
    static auto layout_positions_ready = false;
    static auto hooks_available = false;
    static auto requested_sides = std::array<std::atomic_bool, 2> {};
    static auto active_sides = std::array<std::atomic_bool, 2> {};

    static auto frames = std::array<player_layers, 2> {};
    static thread_local auto frame_resource = static_cast<void*>(nullptr);
    static thread_local auto frame_sides = std::array<bool, 2> {};

    using layer_create_fn = void*(*)(void*, void*, const char*, unsigned, int, unsigned);
    using element_find_fn = void*(*)(void*, const char*);
    static layer_create_fn original_layer_create = nullptr;
    static element_find_fn original_element_find = nullptr;

    auto available() -> bool
    {
        return hooks_available;
    }

    auto update() -> void
    {
        requested_sides[0] = enabled_p1;
        requested_sides[1] = enabled_p2;
    }

    auto reset() -> void
    {
        enabled_p1 = false;
        enabled_p2 = false;
        // An active layout stays intact until the next native frame initialization.
        update();
    }

    static auto single_play() -> bool
    {
        return bm2dx::state != nullptr && bm2dx::state->play_style == 0 &&
               bm2dx::state->game_type != 9;
    }

    template<typename Value>
    static auto write_positions(void* target, const Value& positions) -> void
    {
        auto const guard = util::scoped_page_permissions { target, sizeof(positions), PAGE_READWRITE };
        std::memcpy(target, &positions, sizeof(positions));
    }

    static auto update_layout_positions() -> void
    {
        if (!layout_positions_ready)
            return;

        auto const sides = std::array { active_sides[0].load(), active_sides[1].load() };
        auto shift = std::array<int, 2> {};
        for (auto player = 0; player < 2; ++player)
            shift[player] = sides[player] ? outer_edge_offset(player) : 0;

        auto positions = native_positions;
        for (auto* lanes : { &positions.notes, &positions.beams, &positions.effects })
        {
            // SP contains 1P then 2P, each ordered keys 1-7 then scratch; DP follows untouched.
            for (auto player = 0; player < 2; ++player)
            {
                if (sides[player])
                {
                    auto& columns = lanes->players[player];
                    for (auto& key : columns.keys)
                        key += lane_offset(player, 0);
                    columns.scratch += lane_offset(player, 7);
                }
            }
        }

        write_positions(bm2dx::addr->PLAY_NOTE_POS, positions.notes);
        write_positions(bm2dx::addr->PLAY_BEAM_POS, positions.beams);
        write_positions(bm2dx::addr->PLAY_EFFECT_POS, positions.effects);

        // Full-field overlays move outward but do not follow the scratch/key rearrangement.
        for (auto player = 0; player < 2; ++player)
        {
            positions.judgment.players[player] += shift[player];
            positions.field.single_play.players[player] += shift[player];
        }

        write_positions(bm2dx::addr->PLAY_COVER_POS, positions.field.single_play);
        write_positions(bm2dx::addr->PLAY_JUDGE_POS, positions.judgment);

        for (auto* table : { &positions.indicators.combined, &positions.indicators.keys,
                            &positions.indicators.scratch })
        {
            // Each mode contains SP then DP, with an X/Y pair for each player.
            for (auto mode = std::size_t { 1 }; mode < table->modes.size(); ++mode)
            {
                for (auto player = 0; player < 2; ++player)
                    table->modes[mode].single_play[player].x += shift[player];
            }
        }
        write_positions(bm2dx::addr->PLAY_FS_POSITIONS, positions.indicators);

        // Covers and the red line share native X anchors. Prevent the lazy X copy from undoing ours,
        // without changing the separate height-initialization bit or the native height value.
        auto* red_line = reinterpret_cast<red_line_state*>(bm2dx::addr->PLAY_RED_LINE_STATE);
        auto const guard = util::scoped_page_permissions { red_line, sizeof(*red_line), PAGE_READWRITE };
        red_line->positions = positions.field;
        red_line->initialization_flags = static_cast<red_line_flags>(
            red_line->initialization_flags | red_line_flags::positions_initialized);
    }

    static auto clip_turntable(void* frame, int player) -> void
    {
        if (bm2dx::addr->PLAY_SESSION == nullptr || frames[player].play != frame)
            return;

        auto const session = reinterpret_cast<const bm2dx::play_session_t*>(bm2dx::addr->PLAY_SESSION);
        auto context = bm2dx::turntable_visual_t {};
        std::memcpy(&context, &session->turntables[player], sizeof(context));
        if (context.sprite == nullptr || context.frame != frame)
            return;

        // The emblem is a separate sprite: translating its frame can expose off-screen intro motion.
        auto const displacement = frame_offset(player);
        auto const left = std::max(0, displacement);
        auto const right = std::min(1920, 1920 + displacement);
        auto const bounds = std::array<std::int32_t, 4> { left, 0, right - left, 1080 };
        auto const vtable = *static_cast<void***>(context.sprite);
        reinterpret_cast<std::intptr_t(*)(void*, const std::int32_t*)>(vtable[15])(
            context.sprite, bounds.data());
    }

    static auto layer_create(void* manager, void* resource, const char* name,
                             unsigned layer, int animation, unsigned flags) -> void*
    {
        enum frame_kind { play_frame, gauge_frame, key_support, frame_kind_count };
        static constexpr auto center_layer = 173u;
        static constexpr auto alternate_gauge_layer = 164u;
        static constexpr unsigned layers[frame_kind_count] { 171, 167, 113 };
        static constexpr const char* names[frame_kind_count][2] {
            { "1p_frame", "2p_frame" },
            { "gauge_up_1p", "gauge_up_2p" },
            { "key_support_1p", "key_support_2p" }
        };

        auto player = -1;
          auto kind = play_frame;
        auto const selected_name = name;
          auto const center_frame = name != nullptr && layer == center_layer &&
            (std::strcmp(name, "center_frame_sp") == 0 ||
             std::strcmp(name, "center_frame_2pp") == 0 ||
             std::strcmp(name, "center_frame_dp_1p") == 0 ||
             std::strcmp(name, "center_frame_dp_2p") == 0);

        // Center frames start a fresh layout. Latch SP eligibility here; DP and demos keep no active sides.
        if (center_frame)
        {
            frames = {};
            for (auto& active : active_sides)
                active = false;
            update_layout_positions();
            frame_resource = resource;
            frame_sides = {};
            if (single_play())
                frame_sides = { requested_sides[0].load(), requested_sides[1].load() };
        }

        if (name != nullptr && (frame_sides[0] || frame_sides[1]))
        {
            for (auto category = 0; category < frame_kind_count; ++category)
            {
                if ((layer != layers[category] && !(category == gauge_frame && layer == alternate_gauge_layer)) ||
                    (category != key_support && resource != frame_resource))
                    continue;

                for (auto side = 0; side < 2; ++side)
                {
                    auto const enabled = category == play_frame ? frame_sides[side] : active_sides[side].load();
                    if (enabled &&
                        std::strcmp(selected_name, names[category][side]) == 0)
                    {
                        player = side;
                        kind = static_cast<frame_kind>(category);
                        name = names[category][1 - side];
                    }
                }
            }
        }

        // A missing opposite-side asset falls back to the native layout for that side.
        auto frame = original_layer_create(manager, resource, name, layer, animation, flags);
        if (frame == nullptr && player >= 0)
        {
            frame = original_layer_create(manager, resource, selected_name, layer, animation, flags);
            player = -1;
        }

        if (center_frame && frame == nullptr)
            frame_sides = {};

        // Native layer objects are pooled and may be reused for unrelated assets.
        for (auto& tracked : frames)
        {
            if (tracked.play == frame)
                tracked.play = nullptr;
            if (tracked.gauge == frame)
                tracked.gauge = nullptr;
        }

        if (frame != nullptr && player >= 0)
        {
            auto const vtable = *static_cast<void***>(frame);
            reinterpret_cast<void(*)(void*, int, int)>(vtable[9])(
                frame, frame_offset(player), 0);
            if (kind == play_frame)
                frames[player].play = frame;
            else if (kind == gauge_frame)
                frames[player].gauge = frame;

            if (kind == play_frame)
            {
                active_sides[player] = true;
                update_layout_positions();
            }
        }
        return frame;
    }

    static auto element_find(void* frame, const char* name) -> void*
    {
        if (frame != nullptr && name != nullptr)
        {
            for (auto const& layers : frames)
            {
                if (layers.play != frame && layers.gauge != frame)
                    continue;

                if (std::strcmp(name, "turn_1p") == 0)
                    clip_turntable(frame, 0);
                else if (std::strcmp(name, "turn_2p") == 0)
                    clip_turntable(frame, 1);

                // Swap both directions: native code also probes the absent player's child names.
                auto mapped = std::string { name };
                for (auto index = std::size_t { 0 }; index + 1 < mapped.size(); ++index)
                {
                    if ((mapped[index] == '1' || mapped[index] == '2') && mapped[index + 1] == 'p' &&
                        (index == 0 || mapped[index - 1] == '_') &&
                        (index + 2 == mapped.size() || mapped[index + 2] == '_'))
                        mapped[index] = mapped[index] == '1' ? '2' : '1';
                }
                return original_element_find(frame, mapped.c_str());
            }
        }
        return original_element_find(frame, name);
    }

    auto install_hook() -> void
    {
        // Check that this DLL supports this feature.
        if (bm2dx::addr->PLAY_LAYER_CREATE == nullptr ||
            bm2dx::addr->PLAY_ELEMENT_FIND == nullptr ||
            bm2dx::addr->PLAY_NOTE_POS == nullptr ||
            bm2dx::addr->PLAY_BEAM_POS == nullptr ||
            bm2dx::addr->PLAY_EFFECT_POS == nullptr ||
            bm2dx::addr->PLAY_JUDGE_POS == nullptr ||
            bm2dx::addr->PLAY_COVER_POS == nullptr ||
            bm2dx::addr->PLAY_FS_POSITIONS == nullptr ||
            bm2dx::addr->PLAY_RED_LINE_STATE == nullptr)
            return;

        // Make a copy of the default positions.
        std::memcpy(&native_positions.notes, bm2dx::addr->PLAY_NOTE_POS,
                    sizeof(native_positions.notes));
        std::memcpy(&native_positions.beams, bm2dx::addr->PLAY_BEAM_POS,
                    sizeof(native_positions.beams));
        std::memcpy(&native_positions.effects, bm2dx::addr->PLAY_EFFECT_POS,
                    sizeof(native_positions.effects));
        std::memcpy(&native_positions.judgment, bm2dx::addr->PLAY_JUDGE_POS,
                    sizeof(native_positions.judgment));
        std::memcpy(&native_positions.indicators, bm2dx::addr->PLAY_FS_POSITIONS,
                    sizeof(native_positions.indicators));
        std::memcpy(&native_positions.field, bm2dx::addr->PLAY_COVER_POS,
                    sizeof(native_positions.field));
        layout_positions_ready = true;

        // Install hooks.
        auto const create_result = MH_CreateHook(bm2dx::addr->PLAY_LAYER_CREATE,
            reinterpret_cast<LPVOID>(layer_create), reinterpret_cast<LPVOID*>(&original_layer_create));

        auto const find_result = MH_CreateHook(bm2dx::addr->PLAY_ELEMENT_FIND,
            reinterpret_cast<LPVOID>(element_find), reinterpret_cast<LPVOID*>(&original_element_find));

        // Common startup enables hooks later; remove any partial installation first.
        hooks_available = create_result == MH_OK && find_result == MH_OK;

        // Uninstall hooks if anything failed.
        if (!hooks_available)
        {
            if (create_result == MH_OK)
                MH_RemoveHook(bm2dx::addr->PLAY_LAYER_CREATE);
            if (find_result == MH_OK)
                MH_RemoveHook(bm2dx::addr->PLAY_ELEMENT_FIND);
        }
    }
}