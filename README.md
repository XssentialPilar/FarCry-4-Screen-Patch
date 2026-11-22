# Far Cry 4 Screen Patch

Universal aspect ratio, black-bar removal, FOV scaling, and adaptive UI/HUD patch for the 64-bit PC (Steam / Ubisoft Connect) version of **Far Cry 4**.

Designed to adapt the game viewport and all internal Scaleform/Flash UI elements to any aspect ratio without stretching, distorting, or cropping the image.

## Key Features

- **Zero `.text` Code Modification**: Operates exclusively through read-only AOB symbol discovery and runtime updates to engine `.data` structures and heap-allocated VTables. This preserves `FC64.dll` code integrity and avoids background integrity shutdowns.
- **Complete Black Bar Removal**: Normalizes both `DisplayViewportInfo` structures to `{0, 0, W-1, H-1}`, disables the `2.2:1` ultrawide 3-monitor split, suppresses static black bars, and optionally disables cinematic fade-in bars.
- **Universal Aspect Ratio Support**: Tested across `4:3`, `5:4`, `4:5`, `3:2` (`1.50:1`), `16:10`, `16:9`, `1.85:1`, `2:1`, `18:9`, `19.5:9`, `20:9`, `21:9` (`2.375:1`), `2.35:1` / `2.39:1` / `2.40:1`, `2.76:1`, `9:16`, and `19.5:1`.
- **Adaptive 9-Point UI and HUD Layout**: Dynamically updates active Scaleform `UiAnchorObj` elements across loaded movies using uniform proportional scaling (`min(W / 1280, H / 720)`) for HUD/menus and edge-to-edge fill for full-screen backdrops.
- **Correct Native FOV Scaling**: Automatically sets `UseWidescreenFOV` to **Vert+** (`0`) at `<= 16:9` (preventing horizontal cropping) and **Hor+** (`1`) at `> 16:9` (preserving vertical FOV).
- **Resolution Filter Removal**: Hooks `ModeProvider` via its heap VTable to remove aspect-ratio filtering in the video options menu.

## Screenshots

Captured in-game running on a 16:10 display with full edge-to-edge viewport rendering, no pillarbox or letterbox bars, unstretched Scaleform HUD anchoring, and corrected Vert+ vertical FOV:

### Full-Screen Panoramic Vista and Adaptive HUD Anchoring
![Kyrat valley vista with full viewport and edge-anchored minimap](assets/screenshots/kyrat-vista-16x10.jpg)

### Vertical Architecture and Proportional Viewmodel / Minimap Scaling
![Bell tower full vertical framing and proportional HUD layout](assets/screenshots/bell-tower-hud-16x10.jpg)

### Uncropped Vertical Field of View (Vert+)
![Open sky and vertical FOV framing without black bars](assets/screenshots/sky-eagle-fov-16x10.jpg)

## Installation (Pre-built Release)

1. Download the latest release archive (`FarCry4-Screen-Patch-v2.0.0.zip`) from the **Releases** page.
2. Extract `dinput8.dll` and `FC4_AspectFix.ini` into your Far Cry 4 `bin` folder (next to `FarCry4.exe` and `FC64.dll`), for example:
   ```text
   C:\Program Files (x86)\Steam\steamapps\common\Far Cry 4\bin\
   ```
3. Launch the game normally.

To uninstall, delete `dinput8.dll`, `FC4_AspectFix.ini`, and `FC4_AspectFix.log` from the `bin` folder.

## Configuration (`FC4_AspectFix.ini`)

| Section | Key | Default | Description |
| --- | --- | --- | --- |
| `General` | `Enabled` | `1` | Master toggle (`1` = enabled, `0` = passthrough only). |
| `BlackBars` | `DisableBlackBars` | `1` | Removes 16:9 pillarbox/letterbox clamping, 2.2:1 split, and static black bars. |
| `BlackBars` | `DisableCinematicBlackBars` | `1` | Suppresses scripted cinematic black bar fade-ins (`0` preserves cutscene bars). |
| `UI` | `AdaptiveUI` | `1` | Enables proportional 9-point full-screen Scaleform UI/HUD scaling. |
| `FOV` | `FixFOV` | `1` | Switches between native Vert+ (`<= 16:9`) and Hor+ (`> 16:9`). |
| `Display` | `UnlockResolutions` | `1` | Removes aspect-ratio filtering from the in-game resolution list. |

## Building from Source

### Prerequisites

- Windows x64
- LLVM-MinGW UCRT x86_64 (`x86_64-w64-mingw32-clang++.exe`)

### Build Steps

```powershell
.\build.ps1
```

If `x86_64-w64-mingw32-clang++.exe` is not on your `PATH`, pass `-CompilerPath` explicitly:

```powershell
.\build.ps1 -CompilerPath "C:\path\to\llvm-mingw\bin\x86_64-w64-mingw32-clang++.exe"
```

The compiled `dinput8.dll` and `FC4_AspectFix.ini` will be placed in `dist\`.

## Verification

To verify symbol resolution, `.text` immutability, proxy forwarding, and aspect-ratio math against an installed copy of `FC64.dll` (requires Python 3.10+ and `pefile`):

```powershell
python .\verify_patch.py --bin-dir "C:\Program Files (x86)\Steam\steamapps\common\Far Cry 4\bin"
```

## Project Structure

- `src/dinput8_proxy.cpp` - `dinput8.dll` proxy forwarder, `FarCry4.exe` IAT hook on `FC64.dll!RunGame`, and lifecycle entry points.
- `src/dinput8.def` - Official `dinput8.dll` export table (`@1`-`@6`) and test export.
- `src/hook_engine.h` - Read-only PE `.text` AOB scanner, RIP-relative resolver, IAT helper, and thread-safe file logger.
- `src/patch_main.h` / `src/patch_main.cpp` - Engine symbol resolution, viewport/FOV normalization, heap VTable hooks, and main-thread adaptive UI controller.
- `config/FC4_AspectFix.ini` - Default runtime configuration file.
- `build.ps1` - Reproducible static build script (`-static -static-libgcc -static-libstdc++`).
- `verify_patch.py` - Automated verification suite across 18 aspect ratios.
- `CHANGELOG.md` - Version history.
- `LICENSE` - MIT License.
