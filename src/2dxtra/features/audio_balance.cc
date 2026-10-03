#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <new>
#include <unordered_map>
#include <safetyhook.hpp>
#include "../game.h"
#include "../log.h"
#include "audio_balance.h"

namespace iidxtra::audio_balance
{
    int global_percent = 100;
    int keysound_percent = 100;
    int bgm_percent = 100;

    enum class category { native, keysound, bgm };

    struct sound_queue
    {
        std::array<int, 16> current;
        std::array<int, 16> last;
        bool enabled;
        std::uint8_t padding[3];
        std::array<unsigned, 100> sounds;
        int count;
    };
    static_assert(offsetof(sound_queue, enabled) == 128);
    static_assert(offsetof(sound_queue, sounds) == 132);
    static_assert(offsetof(sound_queue, count) == 532);

    struct queued_sound
    {
        sound_queue* queue = nullptr;
        unsigned sample = 0;
        category kind = category::native;
    };
    struct voice_state
    {
        std::weak_ptr<void> owner;
        category kind;
    };

    // Native MSVC shared_ptr return buffer: {voice, control block}.
    using get_voice_fn = std::shared_ptr<void>* (*)(void*, std::shared_ptr<void>*, unsigned, unsigned);
    static_assert(sizeof(std::shared_ptr<void>) == 16);

    static std::array<SafetyHookMid, 5> hooks;

    static std::atomic_bool installed = false;
    static std::atomic<const char*> failure = nullptr;
    static std::atomic_uint requested = 100u | (100u << 16);
    static std::atomic_int requested_global = 100;
    static std::mutex voices_mutex;
    static std::unordered_map<void*, voice_state> voices;
    static thread_local std::array<queued_sound, 100> queued;

    auto available() -> bool
    {
        return installed.load() && failure.load() == nullptr;
    }

    auto status() -> const char*
    {
        if (const auto message = failure.load())
            return message;
        return installed.load() ? "Live volume balance" : "Audio balance is not installed";
    }

    static auto fail(const char* message) -> void
    {
        const char* expected = nullptr;
        failure.compare_exchange_strong(expected, message);
    }

    auto update() -> void
    {
        if (global_percent < 0 || global_percent > global_max_percent ||
            keysound_percent < 0 || keysound_percent > max_percent ||
            bgm_percent < 0 || bgm_percent > max_percent)
        {
            fail("Audio balance disabled: global volume must be 0%-400%, keysounds/BGM 0%-200%");
            return;
        }
        requested.store(static_cast<unsigned>(keysound_percent) |
                        (static_cast<unsigned>(bgm_percent) << 16));
        requested_global.store(global_percent);
    }

    auto reset() -> void
    {
        global_percent = keysound_percent = bgm_percent = 100;
        update();
    }

    static auto queue_valid(const sound_queue* queue) -> bool
    {
        if (queue->count >= 0 && queue->count <= static_cast<int>(queued.size()))
            return true;
        fail("Audio balance disabled: unexpected native sound queue size");
        return false;
    }

    static auto remember(sound_queue* queue, category kind) -> void
    {
        if (!queue_valid(queue) || queue->count == static_cast<int>(queued.size()))
        {
            fail("Audio balance disabled: unexpected native enqueue behavior");
            return;
        }
        // Both sites follow the sample write and precede the native count increment.
        queued[queue->count] = {queue, queue->sounds[queue->count], kind};
    }

    static auto queue_key(SafetyHookContext& ctx) -> void
    {
        remember(reinterpret_cast<sound_queue*>(ctx.rbx), category::keysound);
    }

    static auto queue_bgm(SafetyHookContext& ctx) -> void
    {
        remember(reinterpret_cast<sound_queue*>(ctx.rcx), category::bgm);
    }

    static auto playback_category(const SafetyHookContext& ctx) -> category
    {
        const auto* stack = reinterpret_cast<const std::uintptr_t*>(ctx.rsp);
        // 010/012: the ordinary play wrapper pushes RBX and reserves 0x20 bytes.
        // Only inspect its caller/queue registers after identifying that wrapper.
        if (stack[0] != reinterpret_cast<std::uintptr_t>(bm2dx::addr->AUDIO_PLAY_RETURN) ||
            stack[6] != reinterpret_cast<std::uintptr_t>(bm2dx::addr->AUDIO_QUEUE_PLAY_RETURN))
            return category::native;

        const auto* queue = reinterpret_cast<const sound_queue*>(ctx.rsi);
        const auto first = reinterpret_cast<std::uintptr_t>(queue->sounds.data());
        if (!queue_valid(queue) || ctx.rdi < first ||
            (ctx.rdi - first) % sizeof(unsigned) != 0 ||
            (ctx.rdi - first) / sizeof(unsigned) >= static_cast<unsigned>(queue->count))
        {
            fail("Audio balance disabled: unexpected native playback cursor");
            return category::native;
        }

        const auto index = (ctx.rdi - first) / sizeof(unsigned);
        const auto entry = queued[index];
        queued[index] = {};
        if (entry.queue != queue || entry.sample != queue->sounds[index] ||
            (static_cast<unsigned>(ctx.rdx) == 2 && entry.sample != static_cast<unsigned>(ctx.r8) + 1))
        {
            fail("Audio balance disabled: sound queue category mismatch");
            return category::native;
        }
        return entry.kind;
    }

    static auto play(SafetyHookContext& ctx) -> void
    {
        if (!available())
            return;
        const auto kind = playback_category(ctx);
        const auto sample = static_cast<unsigned>(ctx.r8);
        // Bank 2 is song audio; the other banks (including queued system sounds) are untouched.
        if (!available() || static_cast<unsigned>(ctx.rdx) != 2 || sample >= 8191)
            return;
        std::shared_ptr<void> voice;
        reinterpret_cast<get_voice_fn>(bm2dx::addr->AUDIO_GET_VOICE_FN)(
            reinterpret_cast<void*>(ctx.rcx), &voice, 2, sample);
        if (!voice)
            return; // Native playback also ignores missing samples.

        const std::lock_guard lock(voices_mutex);
        if (kind == category::native)
        {
            voices.erase(voice.get());
            return;
        }
        if (!voices.contains(voice.get()))
            std::erase_if(voices, [](const auto& entry) { return entry.second.owner.expired(); });
        try
        {
            // Weak ownership rejects recycled addresses without retaining the voice or its PCM.
            voices.insert_or_assign(voice.get(), voice_state {voice, kind});
        }
        catch (const std::bad_alloc&)
        {
            fail("Audio balance disabled: cannot allocate voice tracking");
        }
    }

    static auto mix(SafetyHookContext& ctx) -> void
    {
        if (!available())
            return;
        const auto levels = requested.load();
        auto percent = 100u;
        {
            const std::lock_guard lock(voices_mutex);
            const auto found = voices.find(reinterpret_cast<void*>(ctx.r14 - 8));
            if (found == voices.end() || found->second.owner.expired())
                return;
            percent = found->second.kind == category::keysound ? levels & 0xffff : levels >> 16;
        }
        if (percent == 100)
            return;
        // 010/012: R14 is VoiceImpl's secondary interface (+8); XMM8 is native gain * fade.
        // Change only this temporary, before native pan/group/connection mixing.
        const auto native_gain = *reinterpret_cast<const float*>(ctx.r14 + 0x68);
        const auto multiplier = percent / 100.0f;
        ctx.xmm8.f32[0] *= native_gain * multiplier > 4.0f ? 4.0f / native_gain : multiplier;
    }

    static auto mix_global(SafetyHookContext& ctx) -> void
    {
        if (!available())
            return;
        const auto percent = requested_global.load();
        if (percent == 100)
            return;
        const auto multiplier = percent / 100.0f;
        // The four output graphs use CGainWithHardLimiter after their source mix.
        // Scale gain and both clip bounds together, without changing native node state.
        for (unsigned lane = 0; lane < 4; ++lane)
        {
            ctx.xmm1.f32[lane] *= multiplier;
            ctx.xmm2.f32[lane] *= multiplier;
            ctx.xmm3.f32[lane] *= multiplier;
        }
    }

    auto shutdown() -> void
    {
        installed.store(false);
        for (auto& hook : hooks)
            hook.reset();
        const std::lock_guard lock(voices_mutex);
        voices.clear();
    }

    auto install_hook() -> void
    {
        const auto& a = *bm2dx::addr;
        const std::array targets {
            a.AUDIO_QUEUE_KEY_APPEND, a.AUDIO_QUEUE_BGM_APPEND, a.AUDIO_PLAY_FN, a.AUDIO_MIX_GAIN,
            a.AUDIO_GLOBAL_MIX
        };
        const std::array callbacks {queue_key, queue_bgm, play, mix, mix_global};
        if (!a.AUDIO_QUEUE_PLAY_RETURN || !a.AUDIO_PLAY_RETURN || !a.AUDIO_GET_VOICE_FN ||
            std::ranges::any_of(targets, [](auto target) { return target == nullptr; }))
        {
            fail("Audio balance unavailable for this game version");
            log::init("{}", status());
            return;
        }

        for (std::size_t i = 0; i < hooks.size(); ++i)
        {
            auto hook = SafetyHookMid::create(targets[i], callbacks[i]);
            if (!hook)
            {
                fail("Audio balance unavailable: native hook installation failed");
                log::init("{} ({:#x}, error {})", status(),
                          reinterpret_cast<std::uintptr_t>(targets[i]), static_cast<int>(hook.error().type));
                shutdown();
                return;
            }
            hooks[i] = std::move(*hook);
        }
        installed.store(true);
    }
}
