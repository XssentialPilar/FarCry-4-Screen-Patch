# Changelog

All notable changes to this project will be documented in this file.

## [2.0.0] - 2026-11-22

### Added
- Zero-`.text`-edit runtime architecture via `dinput8.dll` proxy and `FarCry4.exe` IAT hook on `FC64.dll!RunGame`.
- Read-only AOB scanner (`HookEngine::FindPatternInModule`) resolving 10 engine symbols and helpers without modifying `FC64.dll` code pages.
- Full viewport normalization (`DisplayViewportInfo`) removing 16:9 pillarboxing/letterboxing and the `2.2:1` ultrawide 3-monitor split.
- Dynamic `RenderConfig` FOV switching between **Vert+** (`<= 16:9`) and **Hor+** (`> 16:9`).
- Main-thread adaptive 9-point Scaleform UI/HUD layout controller (`ComputeAdaptiveUiLayout`) with uniform proportional scaling (`min(W / 1280, H / 720)`).
- Heap VTable hooks for `CFireUiBlackBars` (static black bars and cinematic fade-ins) and `ModeProvider` (aspect-ratio resolution filter removal).
- Configurable INI settings (`FC4_AspectFix.ini`) and automated verification script (`verify_patch.py`).
