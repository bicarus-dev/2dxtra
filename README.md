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

## Concentration Movie

On `2026081900-010`, enable **Visuals > Play > Concentration Mode Movie** before
starting a song. The game's concentration mode then shows the native demo movie
and song information. Dimming ranges from 0% to 80% in 20% steps.

The top 64-pixel strip is clipped to hide the DEMONSTRATION banner. Normal attract
demos retain their original layout. The option is disabled on builds without
the required subscreen offsets.
