#include <algorithm>
#include <array>
#include <atomic>
#include <cfloat>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <mutex>
#include <optional>
#include <vector>

#include <fmt/format.h>
#include <imgui.h>
#include <safetyhook.hpp>

#include "gauge_percent.h"
#include "../game.h"

namespace iidxtra::gauge_percent
{
    bool enabled = false;

    namespace
    {
        constexpr int gauge_units_per_percent = 50;
        constexpr int gauge_block_units = 100;
        constexpr int gauge_max = 5000;
        constexpr int gauge_stride = 5;
        constexpr int digit_count = 3;

        constexpr float fraction_scale = 0.55f;
        constexpr float percent_label_width = 0.3f;

        // Snapshot of the native units digit, in the game's 1920x1080 layout coordinates.
        struct number_display
        {
            int hundredths;
            int right;
            int top;
            int width;
            int height;
        };

        SafetyHookMid number_hook;
        std::atomic_bool installed = false;
        std::atomic_bool requested = false;

        std::mutex error_mutex;
        std::string error;

        // The native number hook publishes positions; the overlay consumes them once per frame.
        std::mutex display_mutex;
        std::array<std::optional<number_display>, 2> displays;

        // ImGui borrows these bytes for dynamic font baking; keep them alive with the atlas.
        std::vector<char> font_data;
        ImFont* fraction_font = nullptr;

        auto report(const char* message) -> void
        {
            const std::lock_guard lock(error_mutex);
            if (error != message)
                OutputDebugStringA(fmt::format("[Gauge percentage] {}\n", message).c_str());

            error = message;
        }

        auto percentage_hundredths(int raw, int native_percent) -> int
        {
            raw = std::clamp(raw, 0, gauge_max);

            // Native failed/inactive displays override the quantized value with zero.
            if (native_percent == 0 && raw / gauge_block_units != 0)
                raw = 0;

            return raw * 100 / gauge_units_per_percent;
        }

        auto draw_percentage(SafetyHookContext& context) -> void
        {
            if (!requested.load() || !installed.load())
                return;

            // At the native formatter call: RBX is the side, R8 the frame layer, R9 its digit names.
            const auto player = context.rbx;
            if (player >= 2 || !context.r8 || !context.r9 || !context.rsp)
            {
                report("Unexpected native percentage context; native digits retained.");
                return;
            }

            const std::lock_guard lock(display_mutex);
            displays[player].reset();

            // Sixth argument to the native formatter: false means the gauge uses a hyphen.
            auto* visible = reinterpret_cast<std::uint8_t*>(context.rsp + 0x28);
            if (!*visible)
                return; // Keep the native hidden-gauge hyphen.

            const auto& a = *bm2dx::addr;
            const auto find = reinterpret_cast<void* (*)(void*, const char*)>(a.PLAY_ELEMENT_FIND);
            const auto position = reinterpret_cast<void (*)(void*, int*, int*)>(a.ELEMENT_POSITION_FN);
            const auto size = reinterpret_cast<void (*)(void*, int*, int*)>(a.ELEMENT_SIZE_FN);

            const auto names = reinterpret_cast<const char* const*>(context.r9);
            auto* last = find(reinterpret_cast<void*>(context.r8), names[digit_count - 1]);
            if (!last)
            {
                report("Gauge digit anchor unavailable; native digits retained.");
                return;
            }

            int x = 0, y = 0, digit_width = 0, digit_height = 0;
            position(last, &x, &y);
            size(last, &digit_width, &digit_height);
            if (digit_width <= 0 || digit_height <= 0)
            {
                report("Unexpected gauge digit layout; native digits retained.");
                return;
            }

            const int raw = reinterpret_cast<const int*>(a.GAUGE_VALUES)[player * gauge_stride];
            const int hundredths = percentage_hundredths(raw, static_cast<int>(context.rcx));

            // Native frame sprites are positioned by their center, not their top-left corner.
            displays[player] = number_display {hundredths, x + digit_width / 2,
                y - digit_height / 2, digit_width, digit_height};

            // Preserve native digit sprites, but remove the native two-percent quantization.
            context.rcx = hundredths / 100;
        }
    }

    auto available() -> bool
    {
        return installed.load();
    }

    auto status() -> std::string
    {
        const std::lock_guard lock(error_mutex);
        return error;
    }

    auto update() -> void
    {
        requested = enabled;

        const std::lock_guard lock(display_mutex);
        displays = {};
    }
    auto reset() -> void
    {
        enabled = false;
        update();
    }

    auto init_font() -> void
    {
        // Use the same typeface as FAST/SLOW, resolved relative to bm2dx.dll rather than the CWD.
        std::array<wchar_t, 32768> module_path {};
        const auto module = GetModuleHandleW(L"bm2dx.dll");
        const auto length = module ? GetModuleFileNameW(module, module_path.data(),
            static_cast<DWORD>(module_path.size())) : 0;
        if (!length || length >= module_path.size())
        {
            report("Cannot locate the game font; gauge fraction will use the menu font.");
            return;
        }

        const auto path = std::filesystem::path(module_path.data()).parent_path().parent_path() /
            L"data" / L"font" / L"df-heiseigothic-w7.ttc";
        std::ifstream file(path, std::ios::binary | std::ios::ate);
        const auto size = file ? file.tellg() : std::streampos(-1);
        if (size <= 0 || size > std::numeric_limits<int>::max())
        {
            report("Cannot read Heisei Gothic W7; gauge fraction will use the menu font.");
            return;
        }

        font_data.resize(static_cast<std::size_t>(size));
        file.seekg(0);
        if (!file.read(font_data.data(), static_cast<std::streamsize>(font_data.size())))
        {
            report("Failed reading Heisei Gothic W7; gauge fraction will use the menu font.");
            return;
        }

        ImFontConfig config;
        config.FontDataOwnedByAtlas = false;
        fraction_font = ImGui::GetIO().Fonts->AddFontFromMemoryTTF(font_data.data(),
            static_cast<int>(font_data.size()), 16.0f, &config);
        if (!fraction_font)
            report("Failed loading Heisei Gothic W7; gauge fraction will use the menu font.");
    }

    auto render() -> void
    {
        std::array<std::optional<number_display>, 2> frame;
        {
            const std::lock_guard lock(display_mutex);
            frame = displays;
            displays = {};
        }

        if (!requested.load() || !installed.load())
            return;

        const auto screen = ImGui::GetIO().DisplaySize;
        if (screen.x <= 0 || screen.y <= 0)
            return;

        const float sx = screen.x / 1920.0f, sy = screen.y / 1080.0f;
        auto* font = fraction_font ? fraction_font : ImGui::GetFont();
        auto* draw = ImGui::GetBackgroundDrawList();

        const auto digit_bounds = [&](float size)
        {
            auto* baked = font->GetFontBaked(size);
            const float scale = size / baked->Size;
            ImVec4 box {FLT_MAX, FLT_MAX, -FLT_MAX, -FLT_MAX};

            // A shared cell keeps font size, bearings and baseline independent of the current digit.
            for (unsigned char character = '0'; character <= '9'; ++character)
            {
                const auto* glyph = baked->FindGlyph(character);
                box.x = std::min(box.x, glyph->X0 * scale);
                box.y = std::min(box.y, glyph->Y0 * scale);
                box.z = std::max(box.z, glyph->X1 * scale);
                box.w = std::max(box.w, glyph->Y1 * scale);
            }
            return box;
        };

        for (const auto& display : frame)
        {
            if (!display)
                continue;

            const auto fraction = fmt::format("{}", (display->hundredths % 100) / 10);
            const float left = display->right * sx;
            const float right = left + display->width * percent_label_width * sx;
            const float top = display->top * sy;
            const float bottom = (display->top + display->height) * sy;

            // Fit the shared numeric cell by height, then by the narrow PER label's width.
            // Re-measure after each size change because font hinting can change the glyph bounds.
            const float target_height = display->height * fraction_scale * sy;
            float size = target_height;
            auto box = digit_bounds(size);
            size *= target_height / (box.w - box.y);
            box = digit_bounds(size);
            size *= std::min(1.0f, (right - left - 2 * sx) / (box.z - box.x));
            box = digit_bounds(size);

            // PER is baked into the panel texture. Cover just its black-backed label area.
            draw->AddRectFilled({left, top}, {right, bottom}, IM_COL32(0, 0, 0, 255));

            const float baseline = bottom - 2 * sy;
            const ImVec2 origin {std::round((left + right - box.x - box.z) / 2),
                std::round(baseline - box.w)};
            draw->AddText(font, size, origin, IM_COL32(255, 255, 255, 255), fraction.c_str());

            // Position the point independently so it neither shrinks the digit nor moves its baseline.
            // Floor the horizontal coordinate to prevent pixel snapping into the fractional digit.
            const float point_size = size;
            auto* point_font = font->GetFontBaked(point_size);
            const auto* point = point_font->FindGlyph('.');
            const float point_scale = point_size / point_font->Size;
            const ImVec2 point_origin {std::floor(origin.x + box.x - point->X1 * point_scale),
                std::round(baseline - point->Y1 * point_scale)};
            draw->AddText(font, point_size, point_origin, IM_COL32(255, 255, 255, 255), ".");
        }
    }

    auto install_hook() -> void
    {
        const auto& a = *bm2dx::addr;
        if (!a.GAUGE_PERCENT_CALL || !a.GAUGE_VALUES || !a.PLAY_ELEMENT_FIND ||
            !a.ELEMENT_POSITION_FN || !a.ELEMENT_SIZE_FN)
        {
            report("Decimal gauge percentage is unavailable for this game build.");
            return;
        }

        number_hook = safetyhook::create_mid(a.GAUGE_PERCENT_CALL, draw_percentage);
        installed = static_cast<bool>(number_hook);
        if (!installed)
            report("Could not install decimal gauge percentage hook.");
    }

    auto shutdown() -> void
    {
        installed = false;
        number_hook.reset();
    }
}
