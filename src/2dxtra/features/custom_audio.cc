#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <new>
#include <optional>
#include <random>
#include <string_view>
#include <system_error>
#include <safetyhook.hpp>
#include "../game.h"
#include "../log.h"
#include "custom_audio.h"

namespace iidxtra::custom_audio
{
    std::string select_file;
    std::string decide_file;

    // Read-only view of the game's cache. Ownership stays entirely with the native loader.
    struct cached_sound
    {
        void* voice;
        void* owner;
    };
    struct sound_bank
    {
        cached_sound* begin;
        cached_sound* end;
        cached_sound* capacity;
    };
    static_assert(sizeof(cached_sound) == 16 && sizeof(sound_bank) == 24);

    enum class voice_method : std::size_t
    {
        stop = 3,
        is_playing = 6,
        set_gain = 7,
        get_gain = 8,
        set_pan = 9,
        get_pan = 10,
        set_group = 15,
        get_group = 16
    };

    struct channel_state
    {
        std::vector<std::string> files;
        std::string requested_file;
        std::string error;
        std::optional<unsigned> playing_sample;
        std::optional<unsigned> replaced_sample;
    };

    static constexpr std::array<std::string_view, 4> default_layers {
        "syssd_bgm_default_l0", "syssd_bgm_default_l1",
        "syssd_bgm_default_l2", "syssd_bgm_default_l3"
    };

    static constexpr std::array<const char*, 2> audio_directories {
        "song_select_bgm", "song_select_decide"
    };

    static SafetyHookMid play_hook;
    static SafetyHookInline gain_hook;
    static std::atomic_bool installed = false;
    static std::atomic_bool custom_layers = false;
    static std::atomic<const char*> allocation_error = nullptr;
    static std::mutex mutex;
    static std::array<channel_state, 2> channels;
    static std::string scan_error;
    static constexpr char audio_mount[] = "/2dxtra_custom_audio";
    static std::filesystem::path audio_directory;
    static bool mounted = false;
    static int (*fs_mount)(const char*, const char*, const char*, void*) = nullptr;
    static int (*fs_unmount)(const char*) = nullptr;
    static void* sound_manager = nullptr;
    static std::string select_path;
    static std::optional<unsigned> default_sample;
    static std::mt19937 random_engine {std::random_device {}()};

    static auto state(channel kind) -> channel_state&
    {
        return channels[static_cast<std::size_t>(kind)];
    }

    auto files(channel kind) -> const std::vector<std::string>&
    {
        return state(kind).files;
    }

    auto available() -> bool
    {
        return installed.load();
    }

    auto status() -> std::string
    {
        const std::lock_guard lock(mutex);
        if (const auto error = allocation_error.load())
            return error;
        std::string result = scan_error;
        for (const auto& channel : channels)
        {
            if (!channel.error.empty())
                result += (result.empty() ? "" : "\n") + channel.error;
        }
        return result;
    }

    static auto fail(channel kind, const std::string& message) -> void
    {
        const auto error = (kind == channel::music_select ? "Music select: " : "Music decide: ") + message;
        if (state(kind).error != error)
            log::print("[Custom Audio] {}", error);
        state(kind).error = error;
    }

    auto update() -> void
    {
        const std::lock_guard lock(mutex);
        const std::array<std::string_view, 2> selections {select_file, decide_file};
        for (std::size_t i = 0; i < selections.size(); ++i)
        {
            auto& channel = channels[i];
            if (channel.requested_file != selections[i])
                channel.error.clear();
            channel.requested_file = selections[i];
        }
        allocation_error.store(nullptr);
    }

    auto reset() -> void
    {
        select_file.clear();
        decide_file.clear();
        update();
    }

    static auto audio_path(channel kind, std::string_view filename) -> std::string
    {
        return std::string(audio_mount) + "/" + audio_directories[static_cast<std::size_t>(kind)] +
            "/" + std::string(filename);
    }

    static auto path_text(const std::filesystem::path& path) -> std::string
    {
        const auto utf8 = path.u8string();
        return {utf8.begin(), utf8.end()};
    }

    static auto scan() -> void
    {
        scan_error.clear();
        for (std::size_t i = 0; i < channels.size(); ++i)
        {
            auto& list = channels[i].files;
            list.clear();
            const auto folder = audio_directory / audio_directories[i];
            std::error_code error;
            std::filesystem::directory_iterator it(folder, error), end;
            for (; !error && it != end; it.increment(error))
            {
                const bool regular = it->is_regular_file(error);
                if (error)
                    break;
                if (!regular)
                    continue;
                const auto name = path_text(it->path().filename());
                auto lower = name;
                std::transform(lower.begin(), lower.end(), lower.begin(),
                    [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                if (!lower.ends_with(".sd9"))
                    continue;
                list.push_back(name);
            }

            if (error || list.empty())
            {
                list.clear();
                if (!scan_error.empty())
                    scan_error += "\n";
                scan_error += error ? "Cannot scan " + path_text(folder) + ": " + error.message() :
                    "No *.sd9 files found in " + path_text(folder);
            }
            std::sort(list.begin(), list.end());
        }
        if (!scan_error.empty())
            log::print("[Custom Audio] {}", scan_error);
    }

    static auto choose_path(channel kind) -> std::string
    {
        const auto& channel = state(kind);
        const auto& list = channel.files;
        std::string_view filename = channel.requested_file;
        if (filename.empty())
            return {};
        if (kind == channel::music_decide && filename == follow_bgm)
        {
            if (select_path.empty())
                return {};
            // The two folders use matching basenames, including any version/day suffix.
            auto version = std::string_view(select_path).substr(select_path.find_last_of('/') + 1);
            version.remove_suffix(4);
            const auto match = std::find_if(list.begin(), list.end(), [&](const std::string& file)
            {
                auto candidate = std::string_view(file);
                candidate.remove_suffix(4);
                return candidate == version;
            });
            if (match == list.end())
                return {};
            return audio_path(kind, *match);
        }
        if (filename == "*")
        {
            if (list.empty())
            {
                fail(kind, "no SD9 files in 2dxtra_custom/" +
                    std::string(audio_directories[static_cast<std::size_t>(kind)]));
                return {};
            }
            filename = list[std::uniform_int_distribution<std::size_t>(0, list.size() - 1)(random_engine)];
        }
        else if (std::find(list.begin(), list.end(), filename) == list.end())
        {
            fail(kind, "file was not found during the scan: " +
                path_text(audio_directory / audio_directories[static_cast<std::size_t>(kind)] /
                    std::filesystem::u8path(filename)));
            return {};
        }
        return audio_path(kind, filename);
    }

    static auto default_layer(std::string_view name) -> int
    {
        const auto found = std::find(default_layers.begin(), default_layers.end(), name);
        return found == default_layers.end() ? -1 : static_cast<int>(found - default_layers.begin());
    }

    static auto identify(std::string_view name) -> std::optional<channel>
    {
        if (name == "syssd_music_decide_se" ||
            name == "syssd_se_event1_decide" || name == "syssd_bgm_extrastage_phase01_decide")
            return channel::music_decide;
        if (default_layer(name) >= 0 ||
            name == "syssd_bgm_event1" || name == "syssd_bgm_event1_2" ||
            name == "syssd_bgm_event1_3" || name == "syssd_bgm_extrastage_phase01")
            return channel::music_select;
        return std::nullopt;
    }

    template<class R, class... Args>
    static auto voice_call(void* sound, voice_method method, Args... args) -> R
    {
        const auto table = *static_cast<void***>(sound);
        return reinterpret_cast<R (*)(void*, Args...)>(table[static_cast<std::size_t>(method)])(sound, args...);
    }

    static auto slot(unsigned sample) -> cached_sound*
    {
        using get_bank_fn = sound_bank* (*)(unsigned);
        const auto bank = reinterpret_cast<get_bank_fn>(bm2dx::addr->GET_SOUND_ENTRY_FN)(1);
        if (!bank || !bank->begin || sample >= static_cast<std::size_t>(bank->end - bank->begin))
            return nullptr;
        return bank->begin + sample;
    }

    static auto name(unsigned sample) -> std::string_view
    {
        // Native system-sound records are 648 bytes in both supported builds.
        return *reinterpret_cast<const char* const*>(bm2dx::addr->CUSTOM_AUDIO_NAMES + 648ull * sample);
    }

    static auto stop(unsigned sample) -> void
    {
        if (const auto current = slot(sample); current && current->voice)
            voice_call<void>(current->voice, voice_method::stop);
    }

    struct windows_file
    {
        HANDLE handle;
        ~windows_file() { if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle); }
    };

    static auto describe_load_failure(const std::string& path) -> std::string
    {
        auto message = "Native SD9 load failed: " + path + ".";
        if (!path.starts_with(audio_mount))
            return message + " See the game's Sound log for details.";

        const auto relative = std::string_view(path).substr(sizeof(audio_mount));
        const auto physical = (audio_directory / std::filesystem::u8path(relative)).make_preferred();
        message += " Windows file: " + path_text(physical) + ". ";
        const windows_file source {CreateFileW(physical.c_str(), GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)};
        if (source.handle == INVALID_HANDLE_VALUE)
        {
            const auto code = GetLastError();
            return message + fmt::format("Windows cannot open it: {} (error {}).",
                std::system_category().message(static_cast<int>(code)), code);
        }
        LARGE_INTEGER size {};
        if (!GetFileSizeEx(source.handle, &size))
        {
            const auto code = GetLastError();
            return message + fmt::format("Cannot determine file size: {} (Windows error {}).",
                std::system_category().message(static_cast<int>(code)), code);
        }
        message += fmt::format("Size: {} bytes. ", size.QuadPart);
        std::array<std::uint8_t, 4> header {};
        DWORD read = 0;
        if (!ReadFile(source.handle, header.data(), static_cast<DWORD>(header.size()), &read, nullptr))
        {
            const auto code = GetLastError();
            return message + fmt::format("Windows cannot read its header: {} (error {}).",
                std::system_category().message(static_cast<int>(code)), code);
        }
        if (read < header.size())
            return message + "File is empty or truncated (fewer than 4 header bytes).";
        return message + fmt::format("Windows can read the header ({:02X} {:02X} {:02X} {:02X}), "
            "but the native loader rejected the file. Check the game's Sound log for the cause. "
            "Try an ASCII-only folder/filename if it contains non-ASCII characters.",
            header[0], header[1], header[2], header[3]);
    }

    static auto reload(unsigned sample, const std::string& path, channel kind) -> bool
    {
        const auto current = slot(sample);
        if (!current)
        {
            fail(kind, fmt::format("Native sound cache slot {} is unavailable while loading {}", sample, path));
            return false;
        }
        if (path.starts_with(audio_mount) && !mounted)
        {
            // Extended local paths avoid the launcher's D:/E:/F: arcade-drive remapping.
            auto source = audio_directory.wstring();
            if (!source.starts_with(L"\\\\?\\"))
                source = L"\\\\?\\" + source;
            const auto mount_source = path_text(std::filesystem::path(source));
            const auto result = fs_mount(audio_mount, mount_source.c_str(), "fs", nullptr);
            if (result < 0)
            {
                fail(kind, fmt::format("Cannot map {} to {} (AVS code {:#010x}).",
                    path_text(audio_directory), audio_mount, static_cast<std::uint32_t>(result)));
                return false;
            }
            mounted = true;
        }
        const auto had_voice = current->voice != nullptr;
        const auto gain = had_voice ? voice_call<float>(current->voice, voice_method::get_gain) : 1.0f;
        const auto pan = had_voice ? voice_call<float>(current->voice, voice_method::get_pan) : 0.0f;
        const auto group = had_voice ? voice_call<unsigned>(current->voice, voice_method::get_group) : 0u;
        stop(sample);
        using load_fn = bool (*)(void*, unsigned, const char*);
        if (!reinterpret_cast<load_fn>(bm2dx::addr->CUSTOM_AUDIO_LOAD)(sound_manager, sample, path.c_str()))
        {
            fail(kind, describe_load_failure(path));
            return false;
        }
        if (!current->voice)
        {
            fail(kind, fmt::format("Native SD9 loader returned no playback voice for {} (slot {}).", path, sample));
            return false;
        }
        if (had_voice)
        {
            voice_call<void>(current->voice, voice_method::set_gain, gain);
            voice_call<void>(current->voice, voice_method::set_pan, pan);
            voice_call<void>(current->voice, voice_method::set_group, group);
        }
        return true;
    }

    static auto restore(channel kind) -> bool
    {
        auto& sample = state(kind).replaced_sample;
        if (!sample)
            return true;
        if (!reload(*sample, "/data/sound/system/1/" + std::string(name(*sample)) + ".sd9", kind))
            return false;
        sample.reset();
        return true;
    }

    static auto prepare_file(unsigned sample, const std::string& path, channel kind) -> bool
    {
        if (path.empty())
            return restore(kind);

        auto& audio = state(kind);
        if (audio.playing_sample && *audio.playing_sample != sample)
            stop(*audio.playing_sample);
        const bool cache_ready = !audio.replaced_sample || *audio.replaced_sample == sample || restore(kind);
        if (cache_ready && reload(sample, path, kind))
        {
            audio.replaced_sample = sample;
            return true;
        }

        if (kind == channel::music_select)
        {
            select_path.clear();
            custom_layers.store(false);
        }
        if (!restore(kind))
            return false;
        const auto current = slot(sample);
        return current && current->voice;
    }

    static auto route_default_layer(unsigned sample, int layer) -> std::optional<unsigned>
    {
        default_sample = sample - static_cast<unsigned>(layer);
        if (!custom_layers.load())
            return sample;

        // The four native default layers share one custom track, played through layer 0.
        if (layer > 0)
            stop(sample);
        const auto previous = state(channel::music_select).playing_sample;
        const auto last = previous ? slot(*previous) : nullptr;
        if (last && last->voice && voice_call<bool>(last->voice, voice_method::is_playing) &&
            (layer > 0 || *previous != *default_sample))
            return std::nullopt;
        return default_sample;
    }

    static auto skip_playback(SafetyHookContext& ctx) -> void
    {
        // Both profiles have the same 13-byte native lookup/restart sequence.
        ctx.rip = reinterpret_cast<std::uintptr_t>(bm2dx::addr->CUSTOM_AUDIO_START) + 13;
    }

    auto enter_select() -> void
    {
        if (!available())
            return;
        const std::lock_guard lock(mutex);
        try
        {
            const auto was_custom = custom_layers.exchange(false);
            auto& select = state(channel::music_select);
            select.error.clear();
            select_path.clear();
            select_path = choose_path(channel::music_select);
            if (select_path.empty())
                restore(channel::music_select);
            if (was_custom || !select_path.empty())
            {
                if (select.playing_sample)
                    stop(*select.playing_sample);
                if (default_sample)
                    for (unsigned i = 0; i < default_layers.size(); ++i)
                        stop(*default_sample + i);
            }
            custom_layers.store(!select_path.empty());
        }
        catch (const std::bad_alloc&)
        {
            allocation_error.store("Cannot allocate custom music-select audio");
        }
    }

    static auto play(SafetyHookContext& ctx) -> void
    {
        if (!available() || static_cast<unsigned>(ctx.rdi) != 1)
            return;
        auto sample = static_cast<unsigned>(ctx.rbx);
        const auto filename = name(sample);
        const auto match = identify(filename);
        if (!match)
            return;
        const auto kind = *match;
        const std::lock_guard lock(mutex);
        try
        {
            sound_manager = reinterpret_cast<void*>(ctx.rsi);
            const auto layer = default_layer(filename);
            if (layer >= 0)
            {
                const auto target = route_default_layer(sample, layer);
                if (!target)
                {
                    skip_playback(ctx);
                    return;
                }
                if (*target != sample)
                {
                    sample = *target;
                    ctx.rax = reinterpret_cast<std::uintptr_t>(slot(sample));
                }
            }
            if (kind == channel::music_decide)
                state(kind).error.clear();
            const auto path = kind == channel::music_select ? select_path : choose_path(kind);
            if (!prepare_file(sample, path, kind))
            {
                skip_playback(ctx);
                return;
            }
            if (layer <= 0 || !path.empty())
                state(kind).playing_sample = sample;
        }
        catch (const std::bad_alloc&)
        {
            allocation_error.store("Cannot allocate custom audio file state");
            if (kind == channel::music_select)
            {
                select_path.clear();
                custom_layers.store(false);
            }
            skip_playback(ctx);
        }
    }

    static auto layer_gain(void* controller, unsigned layer) -> float
    {
        if (!custom_layers.load() || layer >= default_layers.size())
            return gain_hook.call<float>(controller, layer);
        if (layer != 0)
            return 0.0f;
        float gain = 0.0f;
        for (unsigned i = 0; i < default_layers.size(); ++i)
            gain = std::max(gain, gain_hook.call<float>(controller, i));
        return gain;
    }

    auto shutdown() -> void
    {
        installed.store(false);
        play_hook.reset();
        gain_hook.reset();
        const std::lock_guard lock(mutex);
        custom_layers.store(false);
        restore(channel::music_select);
        restore(channel::music_decide);
        select_path.clear();
        if (mounted)
        {
            const auto result = fs_unmount(audio_mount);
            if (result < 0)
                log::print("[Custom Audio] Cannot unmount {} (AVS code {:#010x})", audio_mount,
                    static_cast<std::uint32_t>(result));
            else
                mounted = false;
        }
    }

    auto install_hook() -> void
    {
        scan_error.clear();
        std::array<wchar_t, 32768> module_path {};
        const auto module = GetModuleHandleW(L"bm2dx.dll");
        const auto length = module ? GetModuleFileNameW(module, module_path.data(),
            static_cast<DWORD>(module_path.size())) : 0;
        if (!length || length >= module_path.size())
        {
            scan_error = "Cannot locate bm2dx.dll for the custom audio directory";
            log::print("[Custom Audio] {}", scan_error);
            return;
        }
        audio_directory = std::filesystem::path(module_path.data()).parent_path().parent_path() / L"2dxtra_custom";
        scan();
        // AVS 2.17.4/2.17.6 exports used by both supported game builds.
        const auto avs = GetModuleHandleW(L"avs2-core.dll");
        if (!avs)
        {
            scan_error = "Cannot locate avs2-core.dll for custom audio";
            log::print("[Custom Audio] {}", scan_error);
            return;
        }
        fs_mount = reinterpret_cast<decltype(fs_mount)>(GetProcAddress(avs, "XCgsqzn000004b"));
        fs_unmount = reinterpret_cast<decltype(fs_unmount)>(GetProcAddress(avs, "XCgsqzn000004c"));
        if (!fs_mount || !fs_unmount)
        {
            scan_error = "Custom audio requires the AVS mount API for supported IIDX 33 builds";
            log::print("[Custom Audio] {}", scan_error);
            return;
        }
        const auto& a = *bm2dx::addr;
        if (!a.CUSTOM_AUDIO_START || !a.CUSTOM_AUDIO_LAYER_GAIN || !a.CUSTOM_AUDIO_LOAD ||
            !a.CUSTOM_AUDIO_NAMES || !a.GET_SOUND_ENTRY_FN)
        {
            scan_error = "Custom audio is unavailable for this game build";
            return;
        }
        gain_hook = safetyhook::create_inline(a.CUSTOM_AUDIO_LAYER_GAIN, layer_gain);
        play_hook = safetyhook::create_mid(a.CUSTOM_AUDIO_START, play);
        if (!gain_hook || !play_hook)
        {
            play_hook.reset();
            gain_hook.reset();
            scan_error = "Could not install custom audio hooks";
            return;
        }
        installed.store(true);
    }
}
