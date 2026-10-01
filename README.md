![](.github/images/screenshot.png)

# 2dxtra
[![Build-MSVC](https://github.com/aixxe/2dxtra/actions/workflows/Build-MSVC.yml/badge.svg)](https://github.com/aixxe/2dxtra/actions/workflows/Build-MSVC.yml)

A multi-hack for arcade beatmania IIDX

- In-game interface with support for keyboard and controller input
- Automatically blocks invalid scores from being sent to network
- Cheat modifiers such as auto-play, regular speed & CN type override
- Ahead of time Urafumen & All-Scratch chart generator based on 2dxplus
- Rate mod implementation from x0.5 to x3.0 chart speed, with optional pitch shift
- Custom chart loader with support for separate score saving via C API
- Novelty modes to combine notes, re-arrange keysounds & swap scratches
- Includes an updated version of [2dxAutoRetry](https://github.com/aixxe/2dxAutoRetry) with additional options
- Ability to increase or decrease each judgement timing window

## Audio controls

The **Audio** tab separates live volume controls from chart sound modifiers.

- **Keysounds** and **Background music** adjust their respective song audio from
  **0% to 200%** in integer steps. Both default to **100%**, which preserves the
  game's native mix; **0%** silences that channel. Changes apply live, including
  to active voices. These are relative gains, not replacements for the game's
  native volume settings. System/menu sounds are left unchanged.
- Values above **100%** amplify audio and may clip. For a different balance,
  reduce the opposite channel instead of amplifying the desired one.
- Live balance supports **IIDX 33 builds 010 and 012**. Unsupported builds or
  hook initialization failures disable the sliders and show a status message.
- The sound category is tracked per voice. When keysounds and background music
  share the same sample/voice, its **latest trigger** determines the channel gain; overlapping
  uses of that shared voice cannot be balanced independently.
- Balance is applied during native mixing, without changing stored voice
  volumes, fades, panning, or routing. Active sounds pick up changes on their
  next mixer update, even without new chart events. Amplification still respects
  the native per-voice gain ceiling.

**Keysound Switch** (Default, Muted, Shuffle, Random!) applies on the **next chart
load**, unlike the live sliders. Muted removes keysound assignments, so raising
the keysound volume cannot restore them; select Default and reload the chart.

The **Mute BGM** checkbox has been removed. Set **Background music** to **0%**
instead. Old `keysound.mute_bgm` settings are ignored and no longer saved.

The live percentages are saved as `audio.keysound_percent` and
`audio.bgm_percent`, with values restricted to 0–200. Reset settings (or an
Event Mode reset) restores both to 100%.
