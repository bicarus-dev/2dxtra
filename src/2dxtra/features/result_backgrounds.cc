#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <iterator>
#include <map>
#include <mutex>
#include <optional>
#include <random>
#include <string_view>
#include <system_error>
#include <safetyhook.hpp>
#include <d3d9.h>
#include <wincodec.h>
#include <wrl/client.h>
#include "../game.h"
#include "../log.h"
#include "result_backgrounds.h"
#include "scratch_flip.h"

namespace iidxtra::result_backgrounds
{
    std::array<std::string, folders.size()> selections;

    namespace
    {
        using Microsoft::WRL::ComPtr;
        constexpr std::int32_t image_width = 1920;
        constexpr std::int32_t image_height = 1080;
        constexpr UINT image_stride = image_width * 4;
        constexpr UINT image_bytes = image_stride * image_height;

        struct background
        {
            void* methods;
            const std::int32_t* category;
            std::byte unused[256];
            void* artwork;
            std::byte unused_280[16];
            void* frame;
        };
        static_assert(offsetof(background, artwork) == 272);
        static_assert(offsetof(background, frame) == 296);

        struct texture_descriptor
        {
            std::int32_t id = -1;
            std::int32_t width = image_width;
            std::int32_t height = image_height;
            float uv[4] {0, 1, 0, 1};
            std::uint32_t unused = 0;
            const char* name;
        };
        static_assert(sizeof(texture_descriptor) == 40);
        static_assert(offsetof(texture_descriptor, name) == 32);

        struct result_state
        {
            void* scene = nullptr;
            background* bg = nullptr;
            std::size_t index = 0;
            std::vector<std::uint8_t> pixels;
            ComPtr<IDirect3DTexture9> texture;
            IDirect3DDevice9* device = nullptr;
            texture_descriptor descriptor {-1, image_width, image_height, {0, 1, 0, 1}, 0, "2dxtra_result_png"};
            bool upload_failed = false;
            void* frame = nullptr;
            void* clip = nullptr;
        };

        std::mutex mutex;
        std::atomic_bool installed = false;
        std::filesystem::path directory;
        std::array<std::map<std::string, std::vector<std::string>>, folders.size()> catalog;
        std::array<std::string, folders.size()> requested;
        std::array<std::string, folders.size()> errors;
        std::string setup_error;
        std::string scan_error;
        result_state current;
        std::mt19937 random_engine {std::random_device {}()};
        SafetyHookInline draw_hook;
        SafetyHookInline cleanup_hook;
        SafetyHookInline reset_hook;
        SafetyHookInline reset_ex_hook;
        int (*next_clip)(int, int) = nullptr;
        int (*set_property)(int, unsigned, std::uintptr_t) = nullptr;
        int (*set_texture)(int, const texture_descriptor*, bool) = nullptr;
        thread_local background* drawing_background = nullptr;

        auto path_text(const std::filesystem::path& path) -> std::string
        {
            const auto text = path.u8string();
            return {text.begin(), text.end()};
        }

        auto fail(std::size_t index, const std::string& message) -> void
        {
            if (errors[index] != message)
                log::print("[Result Backgrounds] {}: {}", folders[index], message);
            errors[index] = message;
        }

        auto choose(std::size_t size) -> std::size_t
        {
            return std::uniform_int_distribution<std::size_t>(0, size - 1)(random_engine);
        }

        auto selected_pool(int mode, int category) -> std::optional<pool>
        {
            // Ordinary Free, Standard, Step Up and Premium Free only; never courses/dan or demos.
            if (mode != 1 && mode != 2 && mode != 5 && mode != 6)
                return std::nullopt;
            switch (category)
            {
                case 0: return pool::fail;
                case 1: return pool::pass;
                case 2: return pool::aaa;
                case 3: return pool::aa;
                case 4: return pool::a;
                default: return std::nullopt; // Native event illustrations take precedence.
            }
        }

        auto checked(HRESULT result, std::size_t index, const char* operation) -> bool
        {
            if (SUCCEEDED(result))
                return true;
            fail(index, fmt::format("{} failed ({:#010x})", operation, static_cast<std::uint32_t>(result)));
            return false;
        }

        auto decode_png(const std::filesystem::path& path, std::size_t index,
            std::vector<std::uint8_t>& pixels) -> bool
        {
            const auto initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            if (initialized != RPC_E_CHANGED_MODE && !checked(initialized, index, "COM initialization"))
                return false;
            struct com_scope
            {
                bool owned;
                ~com_scope() { if (owned) CoUninitialize(); }
            } com {SUCCEEDED(initialized)};
            ComPtr<IWICImagingFactory> factory;
            ComPtr<IWICBitmapDecoder> decoder;
            ComPtr<IWICBitmapFrameDecode> frame;
            ComPtr<IWICFormatConverter> converter;
            if (!checked(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                    IID_PPV_ARGS(factory.GetAddressOf())), index, "PNG decoder creation") ||
                !checked(factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
                    WICDecodeMetadataCacheOnLoad, &decoder), index, "PNG open"))
                return false;
            GUID format {};
            UINT frames = 0, width = 0, height = 0;
            if (!checked(decoder->GetContainerFormat(&format), index, "PNG format lookup") ||
                !checked(decoder->GetFrameCount(&frames), index, "PNG frame count"))
                return false;
            if (format != GUID_ContainerFormatPng || frames != 1)
            {
                fail(index, "Expected a single-frame PNG: " + path_text(path));
                return false;
            }
            if (!checked(decoder->GetFrame(0, &frame), index, "PNG frame decode") ||
                !checked(frame->GetSize(&width, &height), index, "PNG dimensions"))
                return false;
            if (width != image_width || height != image_height)
            {
                fail(index, fmt::format("Expected {}x{} PNG, got {}x{}: {}",
                    image_width, image_height, width, height, path_text(path)));
                return false;
            }
            if (!checked(factory->CreateFormatConverter(&converter), index, "PNG converter creation") ||
                !checked(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppBGRA,
                    WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom), index, "PNG BGRA conversion"))
                return false;
            pixels.resize(image_bytes);
            return checked(converter->CopyPixels(nullptr, image_stride, image_bytes,
                pixels.data()), index, "PNG pixel decode");
        }

        auto clip_handle(const void* clip) -> int
        {
            return *reinterpret_cast<const int*>(static_cast<const std::byte*>(clip) + 512);
        }

        auto restore_current() -> void
        {
            const auto& a = *bm2dx::addr;
            if (current.clip && current.bg->frame == current.frame)
            {
                auto* stock = current.bg->artwork;
                if (stock)
                {
                    const auto restored = reinterpret_cast<std::uint8_t(*)(void*, void*, const char*)>(
                        a.GRAPHIC_BIND_TEXTURE_FN)(current.clip, stock, "result_bg");
                    if (!restored)
                        log::print("[Result Backgrounds] Could not restore native illustration.");
                }
                else
                {
                    for (auto handle = clip_handle(current.clip); handle >= 0; handle = next_clip(handle, 6))
                    {
                        if (set_property(handle, 4103, 0) < 0 || set_property(handle, 4126, 1) < 0)
                        {
                            log::print("[Result Backgrounds] Could not restore native illustration visibility.");
                            return;
                        }
                    }
                }
            }
        }

        auto release_texture() -> void
        {
            if (current.descriptor.id >= 0)
                reinterpret_cast<void(*)(int)>(bm2dx::addr->TEXTURE_UNREGISTER_FN)(current.descriptor.id);
            current.descriptor.id = -1;
            current.texture.Reset();
            current.device = nullptr;
        }

        auto prepare_reset(IDirect3DDevice9* device) -> void
        {
            const std::lock_guard lock(mutex);
            if (device == current.device)
            {
                release_texture();
                current.upload_failed = false;
            }
        }

        auto WINAPI reset_device(IDirect3DDevice9* device, D3DPRESENT_PARAMETERS* parameters) -> HRESULT
        {
            prepare_reset(device);
            return reset_hook.call<HRESULT>(device, parameters);
        }

        auto WINAPI reset_device_ex(IDirect3DDevice9Ex* device, D3DPRESENT_PARAMETERS* parameters,
            D3DDISPLAYMODEEX* mode) -> HRESULT
        {
            prepare_reset(device);
            return reset_ex_hook.call<HRESULT>(device, parameters, mode);
        }

        auto upload(IDirect3DDevice9* device) -> bool
        {
            const auto index = current.index;
            auto** methods = *reinterpret_cast<void***>(device);
            if (!reset_hook)
                reset_hook = safetyhook::create_inline(methods[16], reset_device);
            ComPtr<IDirect3DDevice9Ex> extended;
            const auto queried = device->QueryInterface(IID_PPV_ARGS(&extended));
            if (queried != E_NOINTERFACE && !checked(queried, index, "Direct3D9Ex query"))
                return false;
            if (extended && !reset_ex_hook)
            {
                auto** extended_methods = *reinterpret_cast<void***>(extended.Get());
                reset_ex_hook = safetyhook::create_inline(extended_methods[132], reset_device_ex);
            }
            if (!reset_hook || (extended && !reset_ex_hook))
            {
                fail(index, "Cannot install PNG texture device-reset hooks.");
                return false;
            }
            ComPtr<IDirect3DTexture9> staging, texture;
            if (!checked(device->CreateTexture(image_width, image_height, 1, 0, D3DFMT_A8R8G8B8,
                    D3DPOOL_SYSTEMMEM, &staging, nullptr), index, "PNG staging texture creation") ||
                !checked(device->CreateTexture(image_width, image_height, 1, 0, D3DFMT_A8R8G8B8,
                    D3DPOOL_DEFAULT, &texture, nullptr), index, "PNG GPU texture creation"))
                return false;
            D3DLOCKED_RECT locked {};
            if (!checked(staging->LockRect(0, &locked, nullptr, 0), index, "PNG texture lock"))
                return false;
            for (int y = 0; y < image_height; ++y)
                std::memcpy(static_cast<std::uint8_t*>(locked.pBits) + y * locked.Pitch,
                    current.pixels.data() + y * image_stride, image_stride);
            if (!checked(staging->UnlockRect(0), index, "PNG texture unlock") ||
                !checked(device->UpdateTexture(staging.Get(), texture.Get()), index, "PNG texture upload"))
                return false;
            // The native registry borrows the COM pointer; unregister before releasing our reference.
            const auto id = reinterpret_cast<int(*)(IDirect3DTexture9*)>(bm2dx::addr->TEXTURE_REGISTER_FN)(
                texture.Get());
            if (id < 0)
            {
                fail(index, "Native texture registry is full.");
                return false;
            }
            current.descriptor.id = id;
            current.texture = std::move(texture);
            current.device = device;
            return true;
        }

        auto draw(background* bg) -> void
        {
            const std::lock_guard lock(mutex);
            if (bg != current.bg)
            {
                draw_hook.call<void>(bg);
                return;
            }
            current.clip = nullptr;
            current.frame = nullptr;
            {
                // Reuse the exact temporary clip resolved by the native draw, rather than allocating another.
                struct capture_scope
                {
                    ~capture_scope() { drawing_background = nullptr; }
                } capture;
                drawing_background = bg;
                draw_hook.call<void>(bg);
            }
            // Native result frames are not present during the initial scene transition.
            if (!bg->frame)
                return;
            auto* device = *reinterpret_cast<IDirect3DDevice9**>(bm2dx::addr->D3D9_DEVICE);
            if (device != current.device)
            {
                release_texture();
                current.upload_failed = false;
                current.device = device;
            }
            if (!device)
            {
                fail(current.index, "Direct3D device is unavailable.");
                return;
            }
            if (current.upload_failed)
                return;
            if (!current.texture && !upload(device))
            {
                current.upload_failed = true;
                return;
            }
            auto* clip = current.clip;
            if (!clip)
            {
                fail(current.index, current.frame ? "Native result illustration lookup returned no clip." :
                    "Native result draw did not request its illustration clip.");
                return;
            }
            auto handle = clip_handle(clip);
            if (handle < 0)
            {
                fail(current.index, "Native result illustration handle is unavailable.");
                current.upload_failed = true;
                return;
            }
            // Match the native illustration's centered origin without consulting an IFS dictionary.
            const float center[] {image_width / 2.0f, image_height / 2.0f};
            for (; handle >= 0; handle = next_clip(handle, 6))
            {
                if (set_texture(handle, &current.descriptor, false) < 0 ||
                    set_property(handle, 4123, reinterpret_cast<std::uintptr_t>(center)) < 0 ||
                    set_property(handle, 4103, 1) < 0 ||
                    set_property(handle, 4126, 1) < 0)
                {
                    fail(current.index, "Cannot apply PNG to native result illustration.");
                    restore_current();
                    current.upload_failed = true;
                    return;
                }
            }
            errors[current.index].clear();
        }

        auto cleanup(void* scene, std::uint8_t unload) -> std::intptr_t
        {
            const std::lock_guard lock(mutex);
            const bool leaving_result = scene == current.scene;
            if (leaving_result)
                restore_current();
            const auto result = cleanup_hook.call<std::intptr_t>(scene, unload);
            if (leaving_result)
            {
                release_texture();
                current = {};
            }
            return result;
        }

        auto scan() -> void
        {
            scan_error.clear();
            for (std::size_t i = 0; i < folders.size(); ++i)
            {
                auto& list = catalog[i];
                list.clear();
                const auto folder = directory / folders[i];
                std::error_code error;
                std::filesystem::recursive_directory_iterator it(folder, error), end;
                for (; !error && it != end; it.increment(error))
                {
                    const auto regular = it->is_regular_file(error);
                    if (error)
                        break;
                    if (!regular)
                        continue;
                    auto extension = path_text(it->path().extension());
                    std::transform(extension.begin(), extension.end(), extension.begin(),
                        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                    if (extension == ".png")
                    {
                        const auto relative = it->path().lexically_relative(folder);
                        if (relative.has_parent_path())
                            list[path_text(*relative.begin())].push_back(path_text(relative));
                    }
                }
                if (error)
                {
                    list.clear();
                    if (!scan_error.empty())
                        scan_error += "\n";
                    scan_error += "Cannot scan " + path_text(folder) + ": " + error.message();
                }
                for (auto& [subfolder, images] : list)
                    std::sort(images.begin(), images.end());
            }
            if (!scan_error.empty())
                log::print("[Result Backgrounds] {}", scan_error);
        }
    }

    auto capture_clip(void* frame, const char* name, void* clip) -> void
    {
        if (drawing_background && frame == drawing_background->frame &&
            name && std::string_view(name) == "illust")
        {
            current.clip = clip;
            current.frame = frame;
        }
    }

    auto has_subfolder(pool kind, const std::string& name) -> bool
    {
        const std::lock_guard lock(mutex);
        return catalog[static_cast<std::size_t>(kind)].contains(name);
    }

    auto subfolders(pool kind) -> std::vector<std::string>
    {
        const std::lock_guard lock(mutex);
        std::vector<std::string> names;
        for (const auto& [name, images] : catalog[static_cast<std::size_t>(kind)])
            names.push_back(name);
        return names;
    }

    auto available() -> bool { return installed.load(); }

    auto status() -> std::string
    {
        const std::lock_guard lock(mutex);
        auto result = setup_error;
        if (!scan_error.empty())
            result += (result.empty() ? "" : "\n") + scan_error;
        for (std::size_t i = 0; i < errors.size(); ++i)
            if (!errors[i].empty())
                result += (result.empty() ? "" : "\n") + std::string(folders[i]) + ": " + errors[i];
        return result;
    }

    auto update() -> void
    {
        const std::lock_guard lock(mutex);
        for (std::size_t i = 0; i < selections.size(); ++i)
        {
            // Preserve a previous individual-image selection by selecting its containing group.
            if (!catalog[i].contains(selections[i]))
                for (const auto& [name, images] : catalog[i])
                    if (std::find(images.begin(), images.end(), selections[i]) != images.end())
                    {
                        selections[i] = name;
                        break;
                    }
            if (requested[i] != selections[i])
                errors[i].clear();
            requested[i] = selections[i];
        }
    }

    auto reset() -> void
    {
        selections.fill("");
        update();
    }

    auto enter_result(void* scene) -> void
    {
        if (!available() || !scene)
            return;
        const std::lock_guard lock(mutex);
        if (!available())
            return;
        if (current.scene)
        {
            if (current.scene != scene)
                log::print("[Result Backgrounds] Previous result has not finished graphics cleanup.");
            return;
        }
        auto* bg = reinterpret_cast<background*>(static_cast<std::byte*>(scene) + 256);
        if (!bm2dx::state || !bg->category)
            return;
        const auto kind = selected_pool(bm2dx::state->game_type, *bg->category);
        if (!kind)
            return;
        const auto index = static_cast<std::size_t>(*kind);
        errors[index].clear();
        const auto& selection = requested[index];
        if (selection.empty())
            return;
        const auto& list = catalog[index];
        if (list.empty())
        {
            fail(index, "No subfolders containing PNGs found in " + path_text(directory / folders[index]));
            return;
        }
        auto selected = list.find(selection);
        if (selection != "*" && selected == list.end())
        {
            fail(index, "Selected PNG subfolder is unavailable: " + selection);
            return;
        }
        if (selection == "*")
        {
            selected = list.begin();
            std::advance(selected, choose(list.size()));
        }
        const auto& images = selected->second;
        const auto& file = images[choose(images.size())];
        std::vector<std::uint8_t> pixels;
        if (!decode_png(directory / folders[index] / std::filesystem::u8path(file), index, pixels))
            return;
        current.scene = scene;
        current.bg = bg;
        current.index = index;
        current.pixels = std::move(pixels);
        log::debug("[Result Backgrounds] {}: {}", folders[index], file);
    }

    auto install_hook() -> void
    {
        std::array<wchar_t, 32768> module_path {};
        const auto module = GetModuleHandleW(L"bm2dx.dll");
        const auto length = module ? GetModuleFileNameW(module, module_path.data(),
            static_cast<DWORD>(module_path.size())) : 0;
        if (!length || length >= module_path.size())
            setup_error = "Cannot locate bm2dx.dll for result backgrounds.";
        else
        {
            directory = std::filesystem::path(module_path.data()).parent_path().parent_path() /
                L"2dxtra_custom" / L"result_img";
            scan();
            const auto afp = GetModuleHandleW(L"afp-core.dll");
            next_clip = afp ? reinterpret_cast<decltype(next_clip)>(GetProcAddress(afp, "XCd229cc000069")) : nullptr;
            set_property = afp ? reinterpret_cast<decltype(set_property)>(GetProcAddress(afp, "XCd229cc000073")) : nullptr;
            set_texture = afp ? reinterpret_cast<decltype(set_texture)>(GetProcAddress(afp, "XCd229cc000088")) : nullptr;
            const auto& a = *bm2dx::addr;
            if (!next_clip || !set_property || !set_texture || !a.D3D9_DEVICE ||
                !a.RESULT_BG_DRAW_FN || !a.SCENE_GRAPHICS_CLEANUP_FN ||
                !a.TEXTURE_REGISTER_FN || !a.TEXTURE_UNREGISTER_FN ||
                !a.PLAY_ELEMENT_FIND || !a.GRAPHIC_BIND_TEXTURE_FN || !scratch_flip::available())
                setup_error = "Result backgrounds are unavailable for this game build.";
            else
            {
                draw_hook = safetyhook::create_inline(a.RESULT_BG_DRAW_FN, draw);
                cleanup_hook = safetyhook::create_inline(a.SCENE_GRAPHICS_CLEANUP_FN, cleanup);
                if (draw_hook && cleanup_hook)
                    installed.store(true);
                else
                {
                    draw_hook.reset();
                    cleanup_hook.reset();
                    setup_error = "Could not install result background hooks.";
                }
            }
        }
        if (!setup_error.empty())
            log::print("[Result Backgrounds] {}", setup_error);
    }

    auto shutdown() -> bool
    {
        {
            const std::lock_guard lock(mutex);
            if (current.scene)
            {
                log::print("[Result Backgrounds] Leave the result screen before unloading 2dxtra.");
                return false;
            }
            installed.store(false);
        }
        draw_hook.reset();
        cleanup_hook.reset();
        reset_ex_hook.reset();
        reset_hook.reset();
        return true;
    }
}
