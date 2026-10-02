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
#include <safetyhook.hpp>
#include "../game.h"
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

    static SafetyHookMid play_hook;
    static SafetyHookInline gain_hook;
    static std::atomic_bool installed = false;
    static std::atomic_bool custom_layers = false;
    static std::atomic<const char*> allocation_error = nullptr;
    static std::mutex mutex;
    static std::array<channel_state, 2> channels;
    static std::string scan_error;
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
        if (!scan_error.empty())
            return scan_error;
        std::string result;
        for (const auto& channel : channels)
        {
            if (!channel.error.empty())
                result += (result.empty() ? "" : "\n") + channel.error;
        }
        return result;
    }

    static auto fail(channel kind, const std::string& message) -> void
    {
        state(kind).error = (kind == channel::music_select ? "Music select: " : "Music decide: ") + message;
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

    static auto scan(const std::filesystem::path& directory) -> void
    {
        scan_error.clear();
        for (auto& channel : channels)
            channel.files.clear();
        std::error_code error;
        std::filesystem::directory_iterator it(directory, error), end;
        for (; !error && it != end; it.increment(error))
        {
            const auto is_file = it->is_regular_file(error);
            if (error)
                break;
            if (!is_file)
                continue;
            const auto utf8 = it->path().filename().u8string();
            const std::string name(utf8.begin(), utf8.end());
            auto lower = name;
            std::transform(lower.begin(), lower.end(), lower.begin(),
                [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (!lower.ends_with(".sd9"))
                continue;
            if (lower.starts_with("custom_bgm_"))
                state(channel::music_select).files.push_back(name);
            else if (lower.starts_with("custom_decide_"))
                state(channel::music_decide).files.push_back(name);
        }
        if (error)
        {
            for (auto& channel : channels)
                channel.files.clear();
            scan_error = "Cannot scan " + directory.string() + ": " + error.message();
            return;
        }
        for (auto& channel : channels)
            std::sort(channel.files.begin(), channel.files.end());
        if (state(channel::music_select).files.empty() || state(channel::music_decide).files.empty())
            scan_error = "No " + std::string(state(channel::music_select).files.empty() ?
                "custom_bgm_*.sd9" : "custom_decide_*.sd9") + " files found in " + directory.string();
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
            // Match the full version suffix, excluding the category prefix and SD9 extension.
            auto version = std::string_view(select_path).substr(
                std::string_view("/2dxtra_custom/custom_bgm_").size());
            version.remove_suffix(4);
            const auto match = std::find_if(list.begin(), list.end(), [&](const std::string& file)
            {
                auto candidate = std::string_view(file).substr(std::string_view("custom_decide_").size());
                candidate.remove_suffix(4);
                return candidate == version;
            });
            if (match == list.end())
                return {};
            return "/2dxtra_custom/" + *match;
        }
        if (filename == "*")
        {
            if (list.empty())
            {
                fail(kind, "no matching SD9 files in 2dxtra_custom");
                return {};
            }
            filename = list[std::uniform_int_distribution<std::size_t>(0, list.size() - 1)(random_engine)];
        }
        else if (std::find(list.begin(), list.end(), filename) == list.end())
        {
            fail(kind, "file was not found at boot: " + std::string(filename));
            return {};
        }
        // AVS roots relative game assets at contents, not at the DLL's modules directory.
        return "/2dxtra_custom/" + std::string(filename);
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

    static auto reload(unsigned sample, const std::string& path, channel kind) -> bool
    {
        const auto current = slot(sample);
        if (!current)
        {
            fail(kind, "native sound cache is unavailable");
            return false;
        }
        const auto had_voice = current->voice != nullptr;
        const auto gain = had_voice ? voice_call<float>(current->voice, voice_method::get_gain) : 1.0f;
        const auto pan = had_voice ? voice_call<float>(current->voice, voice_method::get_pan) : 0.0f;
        const auto group = had_voice ? voice_call<unsigned>(current->voice, voice_method::get_group) : 0u;
        stop(sample);
        using load_fn = bool (*)(void*, unsigned, const char*);
        if (!reinterpret_cast<load_fn>(bm2dx::addr->CUSTOM_AUDIO_LOAD)(sound_manager, sample, path.c_str()))
        {
            fail(kind, "native SD9 load failed: " + path);
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
    }

    auto install_hook() -> void
    {
        std::array<wchar_t, 32768> module_path {};
        const auto module = GetModuleHandleW(L"bm2dx.dll");
        const auto length = module ? GetModuleFileNameW(module, module_path.data(),
            static_cast<DWORD>(module_path.size())) : 0;
        if (!length || length >= module_path.size())
        {
            scan_error = "Cannot locate bm2dx.dll for the custom audio scan";
            return;
        }
        const auto contents = std::filesystem::path(module_path.data()).parent_path().parent_path();
        scan(contents / L"2dxtra_custom");
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
