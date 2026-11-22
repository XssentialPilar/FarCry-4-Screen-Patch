#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdint>

struct PatchConfig {
    bool enabled = true;
    bool disableBlackBars = true;
    bool disableCinematicBlackBars = true;
    bool adaptiveUi = true;
    bool fixFov = true;
    bool unlockResolutions = true;
    bool fixVideoAspectRatio = true;
    bool skipPreRecordedVideos = true;
    bool showSkipPrompt = true;
    bool diagnosticLog = false;
};

struct UiLayoutIO {
    double vpW;      // +0x00
    double vpH;      // +0x08
    double safeL;    // +0x10 (in) -> boundsL (out)
    double safeT;    // +0x18 (in) -> boundsT (out)
    double safeR;    // +0x20 (in) -> boundsR (out)
    double safeB;    // +0x28 (in) -> boundsB (out)
    double scaleX;   // +0x30 (out)
    double scaleY;   // +0x38 (out)
    double cachedW;  // +0x40 (out)
    double cachedH;  // +0x48 (out)
    double one;      // +0x50 (out)
    int32_t mode;    // +0x58 (in)
    int32_t pad;     // +0x5c
};

extern "C" {
    void __fastcall ComputeAdaptiveUiLayout(UiLayoutIO* io);
    bool __fastcall NormalizeViewportStruct(uint8_t* vpInfo, uint32_t* outW, uint32_t* outH, float* outAspect);
}

void LoadPatchConfig(HMODULE hSelf, PatchConfig& outConfig);
int InitializeEngineSymbols(HMODULE hSelf, HMODULE hFC64);
void StartAdaptiveController(HMODULE hSelf, HMODULE hFC64);
void TickAdaptiveEngineOnMainThread();
void ShutdownAdaptiveController();
