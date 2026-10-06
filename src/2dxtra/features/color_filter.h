#pragma once

#include <string>

struct IDirect3DDevice9;

namespace iidxtra::color_filter
{
    struct parameters
    {
        float hue;
        float saturation;
        float lightness;

        // Adjust existing hue/saturation rather than replacing them.
        bool relative = false;

        void (*report)(const std::string&) = nullptr;

        // Batched notes have no sprite additive color; regular sprites retain theirs.
        bool clear_additive = false;
    };

    auto begin_color_filter(IDirect3DDevice9* device, const parameters* color) -> void;
    auto end_color_filter(IDirect3DDevice9* device, const parameters* color) -> void;
    auto apply(void* sprite, const parameters* color) -> void;
}
