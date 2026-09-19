![](.github/images/screenshot.png)

# 2dxtra
[![Build-MSVC](https://github.com/aixxe/2dxtra/actions/workflows/Build-MSVC.yml/badge.svg)](https://github.com/aixxe/2dxtra/actions/workflows/Build-MSVC.yml)

A multi-hack for arcade beatmania IIDX

This branch supports IIDX 33 Sparkle Shower `2026081900-010`.
Other builds are not supported.

- In-game interface with support for keyboard and controller input
- Automatically blocks invalid scores from being sent to network
- Cheat modifiers such as auto-play, regular speed & CN type override
- Ahead of time Urafumen & All-Scratch chart generator based on 2dxplus
- Rate mod implementation based on INFINITAS from x0.5 to x3.0 chart speed
- Custom chart loader with support for separate score saving via C API
- Novelty modes to combine notes, re-arrange keysounds & swap scratches
- Includes an updated version of [2dxAutoRetry](https://github.com/aixxe/2dxAutoRetry) with additional options
- Ability to increase or decrease each judgement timing window

Settings can be saved from **Options > Settings > Save settings**. They are stored
in `2dxtra.sqlite` next to the DLL and loaded automatically at game start.
**Reset to default** resets the active options without overwriting saved settings;
press **Save settings** afterward to save the defaults. Event Mode resets also leave
saved settings unchanged. Chart selection and session data are not saved.
