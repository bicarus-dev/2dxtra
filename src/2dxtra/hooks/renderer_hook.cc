#include <atomic>
#include <mutex>
#include <string_view>
#include <MinHook.h>
#include <safetyhook.hpp>
#include "renderer_hook.h"
#include "../game.h"
#include "../gui/gui.h"
#include "../log.h"

namespace iidxtra::renderer_hook
{
    std::once_flag init_once;

    SafetyHookInline present_hook;
    std::atomic_bool overlay_ready = false;
    std::atomic_bool gui_initialized = false;

    IDirect3DDevice9* device_ptr = nullptr;

    LRESULT (*original_wndproc_fn) (void*, HWND, UINT, WPARAM, LPARAM) = nullptr;

    namespace
    {
        auto remove_input_hook() -> void
        {
            const auto status = MH_RemoveHook(bm2dx::addr->WNDPROC_FN);
            if (status != MH_OK)
                log::print("[Renderer] Could not remove input hook: {}", MH_StatusToString(status));
        }

        auto check_target_result(HRESULT result, const char* operation) -> bool
        {
            if (SUCCEEDED(result))
                return true;
            static thread_local HRESULT last_error = D3D_OK;
            static thread_local std::string_view last_operation;
            if (result != last_error || operation != last_operation)
                log::print("[Renderer] {} failed: {:#010x}", operation, static_cast<unsigned>(result));
            last_error = result;
            last_operation = operation;
            return false;
        }

        class scoped_present_target
        {
            IDirect3DDevice9* device;
            IDirect3DSurface9* previous = nullptr;
            IDirect3DSurface9* backbuffer = nullptr;
            D3DVIEWPORT9 viewport {};
            RECT scissor {};
            bool bound = false;

        public:
            explicit scoped_present_target(IDirect3DDevice9* device) : device(device)
            {
                if (!check_target_result(device->GetRenderTarget(0, &previous), "GetRenderTarget") ||
                    !check_target_result(device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &backbuffer), "GetBackBuffer") ||
                    !check_target_result(device->GetViewport(&viewport), "GetViewport") ||
                    !check_target_result(device->GetScissorRect(&scissor), "GetScissorRect") ||
                    !check_target_result(device->SetRenderTarget(0, backbuffer), "SetRenderTarget"))
                    return;
                bound = true;
            }

            scoped_present_target(const scoped_present_target&) = delete;
            auto operator=(const scoped_present_target&) -> scoped_present_target& = delete;

            ~scoped_present_target()
            {
                if (bound)
                {
                    check_target_result(device->SetRenderTarget(0, previous), "Restore render target");
                    check_target_result(device->SetViewport(&viewport), "Restore viewport");
                    check_target_result(device->SetScissorRect(&scissor), "Restore scissor rectangle");
                }
                if (backbuffer)
                    backbuffer->Release();
                if (previous)
                    previous->Release();
            }

            auto ready() const -> bool { return bound; }
        };
    }

    auto present_hook_fn() -> std::intptr_t
    {
        auto* device = *reinterpret_cast<IDirect3DDevice9**>(bm2dx::addr->D3D9_DEVICE);
        if (!device)
            return present_hook.call<std::intptr_t>();

        // OBS fixes its capture size on the first Present, which can occur during boot.
        // Keep the main backbuffer bound across both presentations, before the GUI is ready too.
        const scoped_present_target target(device);
        if (target.ready() && overlay_ready.load())
        {
            device_ptr = device;
            std::call_once(init_once, []
            {
                gui::init();
                gui_initialized = true;
            });
            if (SUCCEEDED(device->BeginScene()))
            {
                gui::begin();
                gui::render();
                gui::end();

                device->EndScene();
            }
        }

        return present_hook.call<std::intptr_t>();
    }

    auto wndproc_hook_fn(void* a1, HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) -> LRESULT
    {
        auto result = original_wndproc_fn(a1, hwnd, msg, wparam, lparam);
        if (gui_initialized.load())
            gui::wndproc(hwnd, msg, wparam, lparam);
        return result;
    }

    auto enable_overlay() -> void { overlay_ready = true; }

    auto install_hook() -> void
    {
        if (!bm2dx::addr->RENDERER_PRESENT_FN || !bm2dx::addr->D3D9_DEVICE || !bm2dx::addr->WNDPROC_FN)
        {
            log::print("[Renderer] Missing presentation/input hook addresses.");
            return;
        }

        // Hook WndProc to capture keyboard/mouse input.
        auto status = MH_CreateHook(bm2dx::addr->WNDPROC_FN, reinterpret_cast<LPVOID>(wndproc_hook_fn),
            reinterpret_cast<void**>(&original_wndproc_fn));
        if (status != MH_OK)
        {
            log::print("[Renderer] Could not create input hook: {}", MH_StatusToString(status));
            return;
        }
        status = MH_EnableHook(bm2dx::addr->WNDPROC_FN);
        if (status != MH_OK)
        {
            log::print("[Renderer] Could not enable input hook: {}", MH_StatusToString(status));
            remove_input_hook();
            return;
        }
        present_hook = safetyhook::create_inline(bm2dx::addr->RENDERER_PRESENT_FN, present_hook_fn);
        if (!present_hook)
        {
            remove_input_hook();
            log::print("[Renderer] Could not install presentation hook.");
        }
    }

    auto uninstall_hook() -> void
    {
        overlay_ready = false;
        gui_initialized = false;
        if (!present_hook)
            return;
        present_hook.reset();
        remove_input_hook();
    }
}