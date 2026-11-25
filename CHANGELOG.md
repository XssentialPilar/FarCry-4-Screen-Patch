# Changelog

All notable changes to this project will be documented in this file.

## [2.3.0] - 2026-11-25

### Fixed

- Expand the legacy centered 16:9 cursor confinement to the full game client area when the patch expands the viewport. The adjustment is scoped to the focused game window and releases on deactivation.
- Keep the main menu and login foreground proportional above their full-screen backdrops. Place the game version below the player ID.
- Center the weapon wheel and its auxiliary item groups, keep mission panels and the save icon proportional, and extend the map and menu gradient layers independently of their controls.

## [2.2.0] - 2026-11-22

### Added
- **Full-Screen Menu, World Map, and Backdrop Black-Bar Removal**:
  - Recursive FireUI `SpriteNode` visibility sweep (`Sym15_SpriteNodeSetVisible`, `Sym16_SpriteNodeInvalidate`) suppresses baked off-stage `1280x200` and `6000x500` letterbox bars inside `PauseRoot` (`lyt_tabBar`), `WorldMap` (`lyt_mcFrontUI -> mcVignetteContainer`, including the 16:9 edge vignette frame), Pause Menu, Shop, Skills, Crafting, Progress, Collections, and Weapon Wheel.
  - Aspect-preserving, origin-invariant `Cover-Fit` scaling for full-screen backdrops including `lyt_mcMainMenuBG` (Main Menu backdrop), `lyt_mcBG` (Loading Screen), `PressStart` (`Press Any Key`), `lyt_mcBack`, `lyt_mcDefaultBG`, and `lyt_mcBackground*`.
  - Proportional stage centering (`min(W / 1280, H / 720)`) for `1280x720` menu containers over `Cover-Fit` backdrops on non-16:9 displays.
- **Pre-Recorded Bink Video Aspect Ratio Fix & Independent Skip**:
  - Expands fullscreen Bink video quads (`g_Mesh2DPool` & `g_Video2DArray`) with aspect-preserving `Cover-Fit` (`[Video] FixVideoAspectRatio=1`).
  - Independent skip for non-looping pre-recorded videos (startup intros via `Mesh2DPool` + `CBinkResource` vtable `Sym17_BinkResourceVTable`, and story cutscenes via `BinkPlayerMgr`) by pressing any keyboard key, mouse button, or XInput controller button (`[Video] SkipPreRecordedVideos=1`).
  - Discrete in-game `F8` toggle with native on-screen prompt/toast via `SubTitleText2` (`[Video] ShowSkipPrompt=1`) persisted directly to `FC4_AspectFix.ini`.
- Expanded read-only AOB discovery to **17/17** unique engine symbols with zero `.text` bytes modified.
- Added `[General] DiagnosticLog` optional logging toggle in `FC4_AspectFix.ini`.

## [2.0.0] - 2026-11-22

### Added
- Zero-`.text`-edit runtime architecture via `dinput8.dll` proxy and `FarCry4.exe` IAT hook on `FC64.dll!RunGame`.
- Read-only AOB scanner (`HookEngine::FindPatternInModule`) resolving 10 engine symbols and helpers without modifying `FC64.dll` code pages.
- Full viewport normalization (`DisplayViewportInfo`) removing 16:9 pillarboxing/letterboxing and the `2.2:1` ultrawide 3-monitor split.
- Dynamic `RenderConfig` FOV switching between **Vert+** (`<= 16:9`) and **Hor+** (`> 16:9`).
- Main-thread adaptive 9-point Scaleform UI/HUD layout controller (`ComputeAdaptiveUiLayout`) with uniform proportional scaling (`min(W / 1280, H / 720)`).
- Heap VTable hooks for `CFireUiBlackBars` (static black bars and cinematic fade-ins) and `ModeProvider` (aspect-ratio resolution filter removal).
- Configurable INI settings (`FC4_AspectFix.ini`) and automated verification script (`verify_patch.py`).
