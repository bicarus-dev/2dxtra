#pragma once

#include <d3d9.h>

namespace iidxtra::renderer_hook
{
    extern IDirect3DDevice9* device_ptr;

    // Presentation binding starts at boot; the menu is enabled after music data initialization.
    auto enable_overlay() -> void;

	auto install_hook() -> void;
	auto uninstall_hook() -> void;
}