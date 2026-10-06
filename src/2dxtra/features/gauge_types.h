#pragma once

#include <array>
#include <cstddef>

namespace iidxtra::gauge
{
    enum class type {
        Default,
        Dan,
        ExDan,
        Erosion,
        Hazard,
        LR2Easy,
        LR2Normal,
        LR2Hard,
        LR2Death,
        LR2PAttack,
        LR2GAttack,
        LR2Dan
    };

    enum class native_gauge { Normal = 0, Easy = 2, Hard = 3, ExHard = 4 };
    enum class palette { Native, Dan, Erosion, ExDan, White, LR2Easy };

    struct type_definition
    {
        type mode;
        const char* name;
        native_gauge base;
        int starting_percent;
        palette color;
    };

    // Order preserves the existing menu/type IDs. Base and starting value apply only to overrides.
    inline constexpr std::array<type_definition, 12> definitions {{
        {type::Default,    "Default",      native_gauge::Normal, 22,  palette::Native},
        {type::Dan,        "Dan",          native_gauge::Hard,   100, palette::Dan},
        {type::ExDan,      "EX Dan",       native_gauge::ExHard, 100, palette::ExDan},
        {type::Erosion,    "Erosion",      native_gauge::ExHard, 100, palette::Erosion},
        {type::Hazard,     "Hazard",       native_gauge::ExHard, 100, palette::White},
        {type::LR2Easy,    "LR2 Easy",     native_gauge::Easy,   20,  palette::LR2Easy},
        {type::LR2Normal,  "LR2 Normal",   native_gauge::Normal, 20,  palette::Native},
        {type::LR2Hard,    "LR2 Hard",     native_gauge::Hard,   100, palette::Native},
        {type::LR2Death,   "LR2 Death",    native_gauge::ExHard, 100, palette::White},
        {type::LR2PAttack, "LR2 P-Attack", native_gauge::ExHard, 100, palette::Native},
        {type::LR2GAttack, "LR2 G-Attack", native_gauge::Hard,   100, palette::Native},
        {type::LR2Dan,     "LR2 Dan",      native_gauge::Hard,   100, palette::Dan}
    }};

    static_assert([] {
        for (std::size_t index = 0; index < definitions.size(); ++index)
            if (static_cast<std::size_t>(definitions[index].mode) != index)
                return false;
        return true;
    }());

    constexpr auto find_definition(type mode) -> const type_definition*
    {
        const auto index = static_cast<std::size_t>(mode);
        return index < definitions.size() ? &definitions[index] : nullptr;
    }

    constexpr auto is_lr2(type mode) -> bool
    {
        switch (mode)
        {
            case type::LR2Easy:
            case type::LR2Normal:
            case type::LR2Hard:
            case type::LR2Death:
            case type::LR2PAttack:
            case type::LR2GAttack:
            case type::LR2Dan:
                return true;
            default:
                return false;
        }
    }

    constexpr auto is_lr2_recovery(type mode) -> bool
    {
        return mode == type::LR2Easy || mode == type::LR2Normal;
    }

    constexpr auto is_dan(type mode) -> bool
    {
        return mode == type::Dan || mode == type::ExDan || mode == type::LR2Dan;
    }

    struct player_options
    {
        type mode = type::Default;
        int dan_start_percent = 100;
        bool dan_keep = false;
        int erosion_level = 1;
    };
}
