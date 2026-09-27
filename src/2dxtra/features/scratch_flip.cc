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
    // Flags exposed to the GUI.
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
        update();
    }

    static auto is_single_play() -> bool
    {
        return bm2dx::state != nullptr && bm2dx::state->play_style == 0;
    }

    template<typename Value>
    static auto write(void* target, const Value& positions) -> void
    {
        auto const guard = util::scoped_page_permissions { target, sizeof(positions), PAGE_READWRITE };
        std::memcpy(target, &positions, sizeof(positions));
    }

    static auto update_layout_positions() -> void
    {
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

        write(bm2dx::addr->PLAY_NOTE_POS, positions.notes);
        write(bm2dx::addr->PLAY_BEAM_POS, positions.beams);
        write(bm2dx::addr->PLAY_EFFECT_POS, positions.effects);

        // Full-field overlays move outward but do not follow the scratch/key rearrangement.
        for (auto player = 0; player < 2; ++player)
        {
            positions.judgment.players[player] += shift[player];
            positions.field.single_play.players[player] += shift[player];
        }

        write(bm2dx::addr->PLAY_COVER_POS, positions.field.single_play);
        write(bm2dx::addr->PLAY_JUDGE_POS, positions.judgment);

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
        write(bm2dx::addr->PLAY_FS_POSITIONS, positions.indicators);

        // Covers and the red line share native X anchors. Prevent the lazy X copy from undoing ours,
        // without changing the separate height-initialization bit or the native height value.
        auto* red_line = reinterpret_cast<red_line_state*>(bm2dx::addr->PLAY_RED_LINE_STATE);
        auto const guard = util::scoped_page_permissions { red_line, sizeof(*red_line), PAGE_READWRITE };
        red_line->positions = positions.field;
        red_line->initialization_flags = static_cast<red_line_flags>(
            red_line->initialization_flags | red_line_flags::positions_initialized);
    }

    static auto clear_layout() -> void
    {
        frames = {};
        frame_resource = nullptr;
        frame_sides = {};
        if (active_sides[0].load() || active_sides[1].load())
        {
            for (auto& active : active_sides)
                active = false;
            update_layout_positions();
        }
    }

    static auto prepare_layout() -> bool
    {
        auto const requested = requested_sides[0].load() || requested_sides[1].load();
        auto const has_layout = frame_sides[0] || frame_sides[1] ||
                                active_sides[0].load() || active_sides[1].load();
        if (!requested && !has_layout)
            return false;

        if (bm2dx::play_session != nullptr && bm2dx::play_session->in_gameplay)
            return has_layout;

        if (requested && is_single_play())
            return true;

        if (has_layout)
            clear_layout();
        return false;
    }

    static auto layer_create(void* manager, void* resource, const char* name,
                             unsigned layer, int animation, unsigned flags) -> void*
    {
        if (!prepare_layout())
            return original_layer_create(manager, resource, name, layer, animation, flags);

        enum frame_kind {
            play_frame,
            gauge_frame,
            key_support,
            get_ready
        };
        struct frame_assets
        {
            frame_kind kind;
            std::array<const char*, 2> names;
        };

        // Assets that we want to swap between 1P and 2P versions.
        // Note that "RETIRE?" sprite is not included here and ends up drawing in the original
        // position since it requires a runtime hook.
        static constexpr frame_assets assets[] {
            { play_frame, { "1p_frame", "2p_frame" } },
            { gauge_frame, { "gauge_up_1p", "gauge_up_2p" } },
            { key_support, { "key_support_1p", "key_support_2p" } },
            { get_ready, { "1p_ready", "2p_ready" } }
        };

        auto const center_frame = name != nullptr &&
            (std::strcmp(name, "center_frame_sp") == 0 ||
             std::strcmp(name, "center_frame_2pp") == 0);

        // Center frames start a fresh SP layout and capture its requested flip settings.
        if (center_frame)
        {
            clear_layout();
            frame_resource = resource;
            frame_sides = { requested_sides[0].load(), requested_sides[1].load() };
        }

        if (!frame_sides[0] && !frame_sides[1])
            return original_layer_create(manager, resource, name, layer, animation, flags);

        auto player = -1;
        auto kind = play_frame;
        auto replacement_name = name;
        if (name != nullptr)
        {
            for (auto const& asset : assets)
            {
                if (asset.kind == play_frame && resource != frame_resource)
                    continue;

                for (auto side = 0; side < 2; ++side)
                {
                    auto const enabled = asset.kind == play_frame ? frame_sides[side] : active_sides[side].load();
                    if (!enabled || std::strcmp(name, asset.names[side]) != 0)
                        continue;

                    player = side;
                    kind = asset.kind;
                    replacement_name = asset.names[1 - side];
                    break;
                }
                if (player >= 0)
                    break;
            }
        }

        // A missing opposite-side asset falls back to the native layout for that side.
        auto frame = original_layer_create(manager, resource, replacement_name, layer, animation, flags);
        if (frame == nullptr && player >= 0)
        {
            frame = original_layer_create(manager, resource, name, layer, animation, flags);
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

        if (frame == nullptr || player < 0)
            return frame;

        auto const vtable = *static_cast<void***>(frame);
        reinterpret_cast<void(*)(void*, int, int)>(vtable[9])(
            frame, frame_offset(player), 0);
        if (kind == play_frame)
        {
            frames[player].play = frame;
            active_sides[player] = true;
            update_layout_positions();
        }
        else if (kind == gauge_frame)
            frames[player].gauge = frame;
        return frame;
    }

    // Swap "1p" and "2p" sprites.
    static auto swap_player_name(const char* name) -> std::string
    {
        auto mapped = std::string { name };
        auto start = std::size_t { 0 };
        while (start < mapped.size())
        {
            auto end = mapped.find('_', start);
            if (end == std::string::npos)
                end = mapped.size();

            if (mapped.compare(start, end - start, "1p") == 0)
                mapped[start] = '2';
            else if (mapped.compare(start, end - start, "2p") == 0)
                mapped[start] = '1';

            start = end + 1;
        }
        return mapped;
    }

    // Hook to swap out sprites when the game looks one up by name.
    static auto element_find(void* frame, const char* name) -> void*
    {
        if (!prepare_layout())
            return original_element_find(frame, name);

        if (frame == nullptr || name == nullptr)
            return original_element_find(frame, name);

        for (auto const& layers : frames)
        {
            if (layers.play != frame && layers.gauge != frame)
                continue;

            auto const mapped = swap_player_name(name);
            return original_element_find(frame, mapped.c_str());
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