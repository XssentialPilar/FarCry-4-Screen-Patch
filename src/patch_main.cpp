#include "patch_main.h"
#include "hook_engine.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <unordered_map>

namespace {

constexpr UINT_PTR kAdaptiveTimerId = 0xFC41;
constexpr UINT kAdaptiveTimerMs = 33;
constexpr double kAppliedMarkerEps = 1e-4;

using FnTreeNext = void*(__fastcall*)(void* node);
using FnIsValid = bool(__fastcall*)(void* gfxObj);
using FnSetProp = uint32_t*(__fastcall*)(void* gfxObj, uint32_t* outStatus, double val);
using FnGetUi = void(__fastcall*)(void* uiSystem, void** outHolder, uint32_t idx);
using FnGetBlackBarsId = uint8_t*(__fastcall*)(void);
using FnLookupService = uint8_t*(__fastcall*)(void* uiMgr, void* idPtr);
using FnGetSupportedModes = void(__fastcall*)(void* thisPtr, void* outVec, uint32_t flags);
using FnStaticBlackBarsSetVis = void(__fastcall*)(void* thisPtr, uint8_t visible);
using FnBlackBarsFadeIn = void(__fastcall*)(void* thisPtr);
using FnBlackBarsFadeOut = void(__fastcall*)(void* thisPtr);

struct EngineSymbols {
    HMODULE hFC64 = nullptr;
    uint8_t** displayManagerPtr = nullptr;       // RVA 0x2dd84c8
    uint8_t** renderConfigPtr = nullptr;         // RVA 0x2dd7e08
    void** modeProviderPtr = nullptr;            // RVA 0x2dd8538
    const void* modeProviderVTableExp = nullptr; // RVA 0x26d4b10
    const void* optionsFilterRetAddr = nullptr;  // RVA 0xfaab36
    uint8_t** uiMovieMgrPtr = nullptr;           // RVA 0x2e22980
    const float* safeFrameFloats = nullptr;      // RVA 0x2d2b94c [L, R, T, B]
    uint8_t** uiSystemPtr = nullptr;             // RVA 0x2e24c58
    FnGetUi fnGetUi = nullptr;                   // RVA 0x83e3c0
    FnGetBlackBarsId fnGetBlackBarsId = nullptr; // RVA 0x87dc80
    FnLookupService fnLookupService = nullptr;   // RVA 0x7248e0
    const void* blackBarsSetVisAddr = nullptr;   // RVA 0x74a990
    const void* blackBarsFadeInAddr = nullptr;   // RVA 0x74a7d0
    FnTreeNext fnTreeNext = nullptr;             // RVA 0x1d822e0
    FnIsValid fnIsValid = nullptr;               // RVA 0x1cb8fa0
    FnSetProp fnSetXScale = nullptr;             // RVA 0x1cb92f0
    FnSetProp fnSetYScale = nullptr;             // RVA 0x1cb93b0
    FnSetProp fnSetX = nullptr;                  // RVA 0x1cb9090
    FnSetProp fnSetY = nullptr;                  // RVA 0x1cb9150
    bool ready = false;
};

struct AnchorCacheEntry {
    void* gfxObj = nullptr;
    double vpW = 0.0;
    double vpH = 0.0;
    double safeL = 0.0;
    double safeT = 0.0;
    double safeR = 0.0;
    double safeB = 0.0;
    double elemX = 0.0;
    double elemY = 0.0;
    int32_t mode = -1;
    int32_t align = -1;
    int32_t scaleFlags = -1;
    int32_t bScaleX = -1;
    int32_t bScaleY = -1;
};

PatchConfig g_Config{};
EngineSymbols g_Sym{};
volatile LONG g_SymbolsResolved = 0;
volatile LONG g_ControllerStarted = 0;
volatile LONG g_ShuttingDown = 0;

// Shadow VTables living in dinput8.dll writable memory (zero edits to FC64.dll .text/.rdata)
void* g_ModeProviderShadowVTable[7]{};
FnGetSupportedModes g_OrigGetSupportedModes = nullptr;
void* g_HookedModeProviderObj = nullptr;

void* g_BlackBarsShadowVTable[35]{};
FnStaticBlackBarsSetVis g_OrigBlackBarsSetVis = nullptr;
FnBlackBarsFadeIn g_OrigBlackBarsFadeIn = nullptr;
FnBlackBarsFadeOut g_OrigBlackBarsFadeOut = nullptr;
void* g_HookedBlackBarsObj = nullptr;

// Subclassed main game window (lock-free atomic WNDPROC storage; zero deadlock risk)
HWND volatile g_SubclassedWnd = nullptr;
WNDPROC volatile g_OrigWndProc = nullptr;
HANDLE g_WatcherThread = nullptr;

std::unordered_map<const void*, AnchorCacheEntry> g_AnchorCache;
uint32_t g_LastLoggedW = 0;
uint32_t g_LastLoggedH = 0;
int32_t g_LastLoggedFovFlag = -1;

inline bool IsValidUserPtr(const void* p) {
    const uintptr_t addr = reinterpret_cast<uintptr_t>(p);
    return addr >= 0x10000ULL && addr < 0x0000800000000000ULL;
}

inline int32_t ReadI32(const uint8_t* p) {
    int32_t v = 0;
    memcpy(&v, p, sizeof(v));
    return v;
}

inline uint64_t RvaOf(HMODULE hMod, const void* ptr) {
    return static_cast<uint64_t>(
        reinterpret_cast<const uint8_t*>(ptr) - reinterpret_cast<const uint8_t*>(hMod));
}

void __fastcall Hook_GetSupportedDisplayModes(void* thisPtr, void* outVec, uint32_t flags) {
    if (g_OrigGetSupportedModes) {
        g_OrigGetSupportedModes(thisPtr, outVec, flags);
    }
    if (!g_Config.unlockResolutions || !IsValidUserPtr(outVec)) {
        return;
    }
    if (__builtin_return_address(0) == g_Sym.optionsFilterRetAddr) {
        auto* vecBase = reinterpret_cast<uint8_t*>(outVec);
        uint8_t* modeData = *reinterpret_cast<uint8_t**>(vecBase);
        const uint32_t count = *reinterpret_cast<const uint32_t*>(vecBase + 8);
        const int32_t selectedAspect = *reinterpret_cast<const int32_t*>(vecBase + 32);
        if (IsValidUserPtr(modeData) && count > 0 && count <= 512) {
            for (uint32_t i = 0; i < count; ++i) {
                *reinterpret_cast<int32_t*>(modeData + static_cast<size_t>(i) * 48 + 44) = selectedAspect;
            }
            static volatile LONG s_loggedModes = 0;
            if (InterlockedCompareExchange(&s_loggedModes, 1, 0) == 0) {
                HookEngine::LogMsg("[LIVE] ModeProvider unlocked all %u resolutions (filter=%d)",
                                   count, selectedAspect);
            }
        }
    }
}

void __fastcall Hook_StaticBlackBars_SetVis(void* thisPtr, uint8_t visible) {
    (void)visible;
    if (g_OrigBlackBarsSetVis) {
        g_OrigBlackBarsSetVis(thisPtr, 0);
    }
    static volatile LONG s_loggedVis = 0;
    if (InterlockedCompareExchange(&s_loggedVis, 1, 0) == 0) {
        HookEngine::LogMsg("[LIVE] BlackBars StaticBlackBars_SetVis suppressed");
    }
}

void __fastcall Hook_BlackBars_FadeIn(void* thisPtr) {
    if (!g_Config.disableCinematicBlackBars && g_OrigBlackBarsFadeIn) {
        g_OrigBlackBarsFadeIn(thisPtr);
        return;
    }
    static volatile LONG s_loggedFade = 0;
    if (InterlockedCompareExchange(&s_loggedFade, 1, 0) == 0) {
        HookEngine::LogMsg("[LIVE] BlackBars FadeIn suppressed");
    }
}

bool TryInstallModeProviderHook() {
    if (!g_Config.unlockResolutions || !g_Sym.modeProviderPtr) {
        return false;
    }
    void* mp = *g_Sym.modeProviderPtr;
    if (!IsValidUserPtr(mp) || mp == g_HookedModeProviderObj) {
        return mp == g_HookedModeProviderObj;
    }
    void** origVt = *reinterpret_cast<void***>(mp);
    if (!IsValidUserPtr(origVt)) {
        return false;
    }
    if (g_Sym.modeProviderVTableExp && origVt != g_Sym.modeProviderVTableExp) {
        return false;
    }
    memcpy(g_ModeProviderShadowVTable, origVt, sizeof(g_ModeProviderShadowVTable));
    g_OrigGetSupportedModes = reinterpret_cast<FnGetSupportedModes>(g_ModeProviderShadowVTable[2]);
    g_ModeProviderShadowVTable[2] = reinterpret_cast<void*>(&Hook_GetSupportedDisplayModes);
    *reinterpret_cast<void***>(mp) = g_ModeProviderShadowVTable;
    g_HookedModeProviderObj = mp;
    HookEngine::LogMsg("[LIVE] ModeProvider heap VTable hook installed on obj=0x%p", mp);
    return true;
}

bool TryInstallBlackBarsHook() {
    if ((!g_Config.disableBlackBars && !g_Config.disableCinematicBlackBars) ||
        !g_Sym.uiSystemPtr || !g_Sym.fnGetUi || !g_Sym.fnGetBlackBarsId || !g_Sym.fnLookupService) {
        return false;
    }
    uint8_t* uiSys = *g_Sym.uiSystemPtr;
    if (!IsValidUserPtr(uiSys)) {
        return false;
    }
    void* holder = nullptr;
    g_Sym.fnGetUi(uiSys, &holder, 0);
    if (!IsValidUserPtr(holder)) {
        return false;
    }
    void* uiMgr = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(holder) + 16);
    if (!IsValidUserPtr(uiMgr)) {
        return false;
    }
    uint8_t* bbIdObj = g_Sym.fnGetBlackBarsId();
    if (!IsValidUserPtr(bbIdObj)) {
        return false;
    }
    uint8_t* bb = g_Sym.fnLookupService(uiMgr, bbIdObj + 28);
    if (!IsValidUserPtr(bb)) {
        return false;
    }
    if (bb == g_HookedBlackBarsObj) {
        return true;
    }

    void** origVt = *reinterpret_cast<void***>(bb);
    if (!IsValidUserPtr(origVt)) {
        return false;
    }
    if (origVt[32] != g_Sym.blackBarsSetVisAddr || origVt[33] != g_Sym.blackBarsFadeInAddr) {
        return false;
    }

    memcpy(g_BlackBarsShadowVTable, origVt, sizeof(g_BlackBarsShadowVTable));
    g_OrigBlackBarsSetVis = reinterpret_cast<FnStaticBlackBarsSetVis>(g_BlackBarsShadowVTable[32]);
    g_OrigBlackBarsFadeIn = reinterpret_cast<FnBlackBarsFadeIn>(g_BlackBarsShadowVTable[33]);
    g_OrigBlackBarsFadeOut = reinterpret_cast<FnBlackBarsFadeOut>(g_BlackBarsShadowVTable[34]);

    if (g_Config.disableBlackBars) {
        g_BlackBarsShadowVTable[32] = reinterpret_cast<void*>(&Hook_StaticBlackBars_SetVis);
    }
    if (g_Config.disableCinematicBlackBars) {
        g_BlackBarsShadowVTable[33] = reinterpret_cast<void*>(&Hook_BlackBars_FadeIn);
    }

    *reinterpret_cast<void***>(bb) = g_BlackBarsShadowVTable;
    g_HookedBlackBarsObj = bb;

    if (g_Config.disableBlackBars && g_OrigBlackBarsSetVis) {
        g_OrigBlackBarsSetVis(bb, 0);
    }
    if (g_Config.disableCinematicBlackBars && g_OrigBlackBarsFadeOut) {
        g_OrigBlackBarsFadeOut(bb);
    }
    HookEngine::LogMsg("[LIVE] BlackBars heap VTable hook installed on obj=0x%p", bb);
    return true;
}

bool NormalizeDisplayAndRenderConfig(uint32_t& outW, uint32_t& outH, float& outAspect) {
    outW = 0;
    outH = 0;
    outAspect = 0.0f;

    if (!g_Sym.displayManagerPtr) {
        return false;
    }
    uint8_t* dm = *g_Sym.displayManagerPtr;
    if (!IsValidUserPtr(dm)) {
        return false;
    }

    // Normalize both DisplayViewportInfo slots (dm + 0x20 and dm + 0x70)
    const uint8_t useAlt = *(dm + 0x68);
    uint8_t* primaryVp = dm + (useAlt ? 0x70 : 0x20);
    uint8_t* secondaryVp = dm + (useAlt ? 0x20 : 0x70);

    uint32_t w2 = 0, h2 = 0;
    float a2 = 0.0f;
    if (g_Config.disableBlackBars) {
        NormalizeViewportStruct(secondaryVp, &w2, &h2, &a2);
    }

    bool ok = false;
    if (g_Config.disableBlackBars) {
        ok = NormalizeViewportStruct(primaryVp, &outW, &outH, &outAspect);
    } else {
        const uint32_t w = *reinterpret_cast<const uint32_t*>(primaryVp + 0x08);
        const uint32_t h = *reinterpret_cast<const uint32_t*>(primaryVp + 0x0c);
        if (w >= 200 && h >= 200 && w <= 16384 && h <= 16384) {
            outW = w;
            outH = h;
            outAspect = static_cast<float>(w) / static_cast<float>(h);
            ok = true;
        }
    }

    if (!ok) {
        return false;
    }

    if (outW != g_LastLoggedW || outH != g_LastLoggedH) {
        g_LastLoggedW = outW;
        g_LastLoggedH = outH;
        HookEngine::LogMsg("[LIVE] Viewport normalized: %ux%u (aspect=%.4f)", outW, outH, outAspect);
    }

    if (g_Sym.renderConfigPtr) {
        uint8_t* cfg = *g_Sym.renderConfigPtr;
        if (IsValidUserPtr(cfg)) {
            if (g_Config.disableBlackBars || g_Config.adaptiveUi) {
                *reinterpret_cast<int32_t*>(cfg + 0x48) = 0;  // WidescreenLetterbox = 0
            }
            if (g_Config.fixFov) {
                constexpr float kBase16x9 = 16.0f / 9.0f;
                // > 16:9 -> UseWidescreenFOV=1 (native Hor+); <= 16:9 -> UseWidescreenFOV=0 (native Vert+)
                const int32_t fovFlag = (outAspect > kBase16x9 + 1e-4f) ? 1 : 0;
                *reinterpret_cast<int32_t*>(cfg + 0x4c) = fovFlag;
                if (fovFlag != g_LastLoggedFovFlag) {
                    g_LastLoggedFovFlag = fovFlag;
                    HookEngine::LogMsg("[LIVE] FOV mode set: %s (aspect=%.4f UseWidescreenFOV=%d)",
                                       fovFlag ? "Hor+" : "Vert+", outAspect, fovFlag);
                }
            }
        }
    }

    return true;
}

void ApplyAdaptiveUiAnchors(double vpW, double vpH) {
    if (!g_Config.adaptiveUi || !g_Sym.uiMovieMgrPtr || !g_Sym.fnTreeNext ||
        !g_Sym.fnIsValid || !g_Sym.fnSetXScale || !g_Sym.fnSetYScale ||
        !g_Sym.fnSetX || !g_Sym.fnSetY) {
        return;
    }
    uint8_t* mgr = *g_Sym.uiMovieMgrPtr;
    if (!IsValidUserPtr(mgr)) {
        return;
    }

    const uint32_t movieCount = *reinterpret_cast<const uint32_t*>(mgr + 16);
    void** movieItems = *reinterpret_cast<void***>(mgr + 8);
    if (!IsValidUserPtr(movieItems) || movieCount == 0 || movieCount > 256) {
        return;
    }

    // Compute pixel safe bounds matching UiAnchorUpdate (0x749efd..0x749f25)
    double safeL = 0.0;
    double safeT = 0.0;
    double safeR = vpW;
    double safeB = vpH;
    if (IsValidUserPtr(g_Sym.safeFrameFloats)) {
        const float sfL = g_Sym.safeFrameFloats[0];
        const float sfR = g_Sym.safeFrameFloats[1];
        const float sfT = g_Sym.safeFrameFloats[2];
        const float sfB = g_Sym.safeFrameFloats[3];
        if (sfL >= 0.0f && sfR <= 1.0f && sfT >= 0.0f && sfB <= 1.0f && sfR > sfL && sfB > sfT) {
            constexpr double kOverscanMargin = 0.07500000298023224;
            safeL = std::clamp((static_cast<double>(sfL) - kOverscanMargin) * vpW, 0.0, vpW);
            safeR = std::clamp((static_cast<double>(sfR) + kOverscanMargin) * vpW, 0.0, vpW);
            safeT = std::clamp((static_cast<double>(sfT) - kOverscanMargin) * vpH, 0.0, vpH);
            safeB = std::clamp((static_cast<double>(sfB) + kOverscanMargin) * vpH, 0.0, vpH);
        }
    }

    static double s_lastUiW = 0.0;
    static double s_lastUiH = 0.0;
    if (std::fabs(s_lastUiW - vpW) > 0.5 || std::fabs(s_lastUiH - vpH) > 0.5 || g_AnchorCache.size() > 4096) {
        s_lastUiW = vpW;
        s_lastUiH = vpH;
        g_AnchorCache.clear();
    }

    for (uint32_t m = 0; m < movieCount; ++m) {
        auto* mov = reinterpret_cast<uint8_t*>(movieItems[m]);
        if (!IsValidUserPtr(mov)) {
            continue;
        }
        uint8_t* anchorMgr = *reinterpret_cast<uint8_t**>(mov + 24);
        if (!IsValidUserPtr(anchorMgr)) {
            continue;
        }
        uint8_t* treeHead = anchorMgr + 24;
        const uint32_t anchorCount = *reinterpret_cast<const uint32_t*>(treeHead + 32);
        if (anchorCount == 0 || anchorCount > 1024) {
            continue;
        }

        void* node = *reinterpret_cast<void**>(treeHead);
        uint32_t visited = 0;
        while (IsValidUserPtr(node) && node != treeHead && visited < anchorCount + 8) {
            ++visited;
            auto* obj = *reinterpret_cast<uint8_t**>(reinterpret_cast<uint8_t*>(node) + 40);
            if (IsValidUserPtr(obj)) {
                auto* gfxWrap = *reinterpret_cast<uint8_t**>(obj + 0x08);
                if (IsValidUserPtr(gfxWrap)) {
                    void* gfxObj = *reinterpret_cast<void**>(gfxWrap + 0x10);
                    if (IsValidUserPtr(gfxObj) && g_Sym.fnIsValid(gfxObj)) {
                        const double elemX = *reinterpret_cast<const double*>(obj + 0x10);
                        const double elemY = *reinterpret_cast<const double*>(obj + 0x18);
                        const double curCachedW = *reinterpret_cast<const double*>(obj + 0x20);
                        const double curCachedH = *reinterpret_cast<const double*>(obj + 0x28);
                        const int32_t mode = *reinterpret_cast<const int32_t*>(obj + 0x30);
                        const int32_t scaleFlags = *reinterpret_cast<const int32_t*>(obj + 0x34);
                        const int32_t align = *reinterpret_cast<const int32_t*>(obj + 0x38);
                        const int32_t bScaleX = *reinterpret_cast<const int32_t*>(obj + 0x3c);
                        const int32_t bScaleY = *reinterpret_cast<const int32_t*>(obj + 0x40);

                        UiLayoutIO io{};
                        io.vpW = vpW;
                        io.vpH = vpH;
                        io.safeL = safeL;
                        io.safeT = safeT;
                        io.safeR = safeR;
                        io.safeB = safeB;
                        io.mode = mode;
                        ComputeAdaptiveUiLayout(&io);

                        const double markedW = io.cachedW + kAppliedMarkerEps;
                        const double markedH = io.cachedH;

                        auto it = g_AnchorCache.find(obj);
                        bool needsApply = (it == g_AnchorCache.end());
                        if (!needsApply) {
                            const AnchorCacheEntry& c = it->second;
                            if (c.gfxObj != gfxObj ||
                                std::fabs(c.vpW - vpW) > 0.5 ||
                                std::fabs(c.vpH - vpH) > 0.5 ||
                                std::fabs(c.safeL - safeL) > 0.5 ||
                                std::fabs(c.safeT - safeT) > 0.5 ||
                                std::fabs(c.safeR - safeR) > 0.5 ||
                                std::fabs(c.safeB - safeB) > 0.5 ||
                                std::fabs(c.elemX - elemX) > 1e-4 ||
                                std::fabs(c.elemY - elemY) > 1e-4 ||
                                c.mode != mode || c.align != align ||
                                c.scaleFlags != scaleFlags ||
                                c.bScaleX != bScaleX || c.bScaleY != bScaleY ||
                                std::fabs(curCachedW - markedW) > 1e-5 ||
                                std::fabs(curCachedH - markedH) > 1e-4) {
                                needsApply = true;
                            }
                        }

                        if (needsApply) {
                            *reinterpret_cast<double*>(obj + 0x20) = markedW;
                            *reinterpret_cast<double*>(obj + 0x28) = markedH;

                            uint32_t status = 0;
                            if (scaleFlags == 2 || scaleFlags == 3) {
                                g_Sym.fnSetXScale(gfxObj, &status, io.scaleX);
                            }
                            if (scaleFlags == 1 || scaleFlags == 3) {
                                g_Sym.fnSetYScale(gfxObj, &status, io.scaleY);
                            }

                            const double posScaleX = (bScaleX != 0) ? io.scaleX : 1.0;
                            const double posScaleY = (bScaleY != 0) ? io.scaleY : 1.0;

                            if (align >= 0 && align <= 8) {
                                double outX = elemX;
                                switch (align % 3) {
                                    case 0:  // LEFT
                                        outX = io.safeL + posScaleX * elemX;
                                        break;
                                    case 1:  // CENTER
                                        outX = io.safeL + 0.5 * io.cachedW - posScaleX * (640.0 - elemX);
                                        break;
                                    case 2:  // RIGHT
                                        outX = io.safeR - posScaleX * (1280.0 - elemX);
                                        break;
                                }

                                double outY = elemY;
                                switch (align / 3) {
                                    case 0:  // TOP
                                        outY = io.safeT + posScaleY * elemY;
                                        break;
                                    case 1:  // MIDDLE
                                        outY = io.safeT + 0.5 * io.cachedH - posScaleY * (360.0 - elemY);
                                        break;
                                    case 2:  // BOTTOM
                                        outY = io.safeB - posScaleY * (720.0 - elemY);
                                        break;
                                }

                                g_Sym.fnSetX(gfxObj, &status, outX);
                                g_Sym.fnSetY(gfxObj, &status, outY);
                            }

                            AnchorCacheEntry entry{};
                            entry.gfxObj = gfxObj;
                            entry.vpW = vpW;
                            entry.vpH = vpH;
                            entry.safeL = safeL;
                            entry.safeT = safeT;
                            entry.safeR = safeR;
                            entry.safeB = safeB;
                            entry.elemX = elemX;
                            entry.elemY = elemY;
                            entry.mode = mode;
                            entry.align = align;
                            entry.scaleFlags = scaleFlags;
                            entry.bScaleX = bScaleX;
                            entry.bScaleY = bScaleY;
                            g_AnchorCache[obj] = entry;

                            static volatile LONG s_loggedModeMask = 0;
                            const LONG bit = (mode >= 0 && mode <= 5) ? (1L << mode) : (1L << 6);
                            if ((InterlockedOr(&s_loggedModeMask, bit) & bit) == 0) {
                                HookEngine::LogMsg(
                                    "[LIVE] AdaptiveUI applied (vp=%.0fx%.0f mode=%d scale=%.4fx%.4f bounds=[%.0f,%.0f,%.0f,%.0f])",
                                    vpW, vpH, mode, io.scaleX, io.scaleY,
                                    io.safeL, io.safeT, io.safeR, io.safeB);
                            }
                        }
                    }
                }
            }
            node = g_Sym.fnTreeNext(node);
        }
    }
}

LRESULT CALLBACK Hook_WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_TIMER && wParam == kAdaptiveTimerId) {
        TickAdaptiveEngineOnMainThread();
        return 0;
    }
    if (msg == WM_SIZE || msg == WM_DISPLAYCHANGE || msg == WM_ACTIVATEAPP) {
        TickAdaptiveEngineOnMainThread();
    }

    WNDPROC orig = reinterpret_cast<WNDPROC>(
        InterlockedCompareExchangePointer(reinterpret_cast<PVOID volatile*>(&g_OrigWndProc), nullptr, nullptr));
    HWND subWnd = reinterpret_cast<HWND>(
        InterlockedCompareExchangePointer(reinterpret_cast<PVOID volatile*>(&g_SubclassedWnd), nullptr, nullptr));

    if (msg == WM_DESTROY && hWnd == subWnd) {
        KillTimer(hWnd, kAdaptiveTimerId);
        if (orig) {
            SetWindowLongPtrW(hWnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(orig));
        }
        InterlockedExchangePointer(reinterpret_cast<PVOID volatile*>(&g_SubclassedWnd), nullptr);
        InterlockedExchangePointer(reinterpret_cast<PVOID volatile*>(&g_OrigWndProc), nullptr);
    }

    if (orig && orig != &Hook_WndProc) {
        return CallWindowProcW(orig, hWnd, msg, wParam, lParam);
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

struct FindBestWindowCtx {
    DWORD pid = 0;
    HWND bestHwnd = nullptr;
    int64_t bestArea = 0;
    bool bestTitleMatch = false;
};

BOOL CALLBACK EnumGameWindowsProc(HWND hWnd, LPARAM lParam) {
    auto* ctx = reinterpret_cast<FindBestWindowCtx*>(lParam);
    DWORD wndPid = 0;
    GetWindowThreadProcessId(hWnd, &wndPid);
    if (wndPid != ctx->pid) {
        return TRUE;
    }
    if (!IsWindowVisible(hWnd) || GetParent(hWnd) != nullptr) {
        return TRUE;
    }
    RECT rc{};
    if (!GetClientRect(hWnd, &rc)) {
        return TRUE;
    }
    const int64_t w = static_cast<int64_t>(rc.right - rc.left);
    const int64_t h = static_cast<int64_t>(rc.bottom - rc.top);
    if (w < 320 || h < 200) {
        return TRUE;
    }
    wchar_t title[128]{};
    DWORD_PTR textRes = 0;
    SendMessageTimeoutW(
        hWnd, WM_GETTEXT, 128, reinterpret_cast<LPARAM>(title),
        SMTO_ABORTIFHUNG | SMTO_NORMAL, 50, &textRes);
    const bool titleMatch = (wcsstr(title, L"Far Cry") != nullptr);
    const int64_t area = w * h;

    if (!ctx->bestHwnd ||
        (titleMatch && !ctx->bestTitleMatch) ||
        (titleMatch == ctx->bestTitleMatch && area > ctx->bestArea)) {
        ctx->bestHwnd = hWnd;
        ctx->bestArea = area;
        ctx->bestTitleMatch = titleMatch;
    }
    return TRUE;
}

DWORD WINAPI WindowAttachThreadProc(LPVOID) {
    const DWORD selfPid = GetCurrentProcessId();
    while (InterlockedCompareExchange(&g_ShuttingDown, 0, 0) == 0) {
        HWND curWnd = reinterpret_cast<HWND>(
            InterlockedCompareExchangePointer(reinterpret_cast<PVOID volatile*>(&g_SubclassedWnd), nullptr, nullptr));
        const bool hasSubclassed = (curWnd != nullptr && IsWindow(curWnd));
        if (!hasSubclassed) {
            uint32_t w = 0, h = 0;
            float aspect = 0.0f;
            NormalizeDisplayAndRenderConfig(w, h, aspect);
            TryInstallModeProviderHook();
        }

        FindBestWindowCtx ctx{};
        ctx.pid = selfPid;
        EnumWindows(EnumGameWindowsProc, reinterpret_cast<LPARAM>(&ctx));

        if (ctx.bestHwnd && ctx.bestHwnd != curWnd) {
            // Read window title BEFORE subclassing (avoid cross-thread SendMessage after swap)
            wchar_t title[128]{};
            DWORD_PTR textRes = 0;
            SendMessageTimeoutW(
                ctx.bestHwnd, WM_GETTEXT, 128, reinterpret_cast<LPARAM>(title),
                SMTO_ABORTIFHUNG | SMTO_NORMAL, 50, &textRes);
            char titleAnsi[128]{};
            WideCharToMultiByte(CP_ACP, 0, title, -1, titleAnsi, sizeof(titleAnsi), nullptr, nullptr);

            // Restore previous window if switching from splash to main D3D11 window
            WNDPROC prevOrig = reinterpret_cast<WNDPROC>(
                InterlockedCompareExchangePointer(reinterpret_cast<PVOID volatile*>(&g_OrigWndProc), nullptr, nullptr));
            if (curWnd && IsWindow(curWnd) && prevOrig && prevOrig != &Hook_WndProc) {
                KillTimer(curWnd, kAdaptiveTimerId);
                SetWindowLongPtrW(curWnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(prevOrig));
            }

            // Store original WNDPROC atomically BEFORE swapping so Hook_WndProc always sees it immediately
            WNDPROC targetOrig = reinterpret_cast<WNDPROC>(GetWindowLongPtrW(ctx.bestHwnd, GWLP_WNDPROC));
            if (targetOrig && targetOrig != &Hook_WndProc) {
                InterlockedExchangePointer(reinterpret_cast<PVOID volatile*>(&g_OrigWndProc),
                                           reinterpret_cast<PVOID>(targetOrig));
                InterlockedExchangePointer(reinterpret_cast<PVOID volatile*>(&g_SubclassedWnd),
                                           reinterpret_cast<PVOID>(ctx.bestHwnd));
                SetWindowLongPtrW(ctx.bestHwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&Hook_WndProc));
                SetTimer(ctx.bestHwnd, kAdaptiveTimerId, kAdaptiveTimerMs, nullptr);
                HookEngine::LogMsg("[LIVE] Attached main-thread adaptive timer to HWND=0x%p ('%s')",
                                   ctx.bestHwnd, titleAnsi);
            }
        }

        Sleep(250);
    }
    return 0;
}

}  // namespace

void LoadPatchConfig(HMODULE hSelf, PatchConfig& outConfig) {
    outConfig = PatchConfig{};
    wchar_t dir[MAX_PATH]{};
    HookEngine::ResolveModuleDir(hSelf, dir, MAX_PATH);
    wchar_t iniPath[MAX_PATH]{};
    _snwprintf_s(iniPath, MAX_PATH, _TRUNCATE, L"%s\\FC4_AspectFix.ini", dir);

    outConfig.enabled = GetPrivateProfileIntW(L"General", L"Enabled", 1, iniPath) != 0;
    outConfig.disableBlackBars = GetPrivateProfileIntW(L"BlackBars", L"DisableBlackBars", 1, iniPath) != 0;
    outConfig.disableCinematicBlackBars =
        GetPrivateProfileIntW(L"BlackBars", L"DisableCinematicBlackBars", 1, iniPath) != 0;
    outConfig.adaptiveUi = GetPrivateProfileIntW(L"UI", L"AdaptiveUI", 1, iniPath) != 0;
    outConfig.fixFov = GetPrivateProfileIntW(L"FOV", L"FixFOV", 1, iniPath) != 0;
    outConfig.unlockResolutions =
        GetPrivateProfileIntW(L"Display", L"UnlockResolutions", 1, iniPath) != 0;
}

extern "C" void __fastcall ComputeAdaptiveUiLayout(UiLayoutIO* io) {
    if (!io) {
        return;
    }
    const double vpW = (io->vpW > 1.0) ? io->vpW : 1.0;
    const double vpH = (io->vpH > 1.0) ? io->vpH : 1.0;

    double sL = std::clamp(io->safeL, 0.0, vpW);
    double sT = std::clamp(io->safeT, 0.0, vpH);
    double sR = std::clamp(io->safeR, 0.0, vpW);
    double sB = std::clamp(io->safeB, 0.0, vpH);
    if (sR <= sL + 1.0) {
        sL = 0.0;
        sR = vpW;
    }
    if (sB <= sT + 1.0) {
        sT = 0.0;
        sB = vpH;
    }

    const double safeW = sR - sL;
    const double safeH = sB - sT;
    constexpr double kInvRefW = 1.0 / 1280.0;
    constexpr double kInvRefH = 1.0 / 720.0;

    io->one = 1.0;

    switch (io->mode) {
        case 0: {
            io->safeL = 0.0;
            io->safeT = 0.0;
            io->safeR = vpW;
            io->safeB = vpH;
            io->cachedW = vpW;
            io->cachedH = vpH;
            io->scaleX = vpW * kInvRefW;
            io->scaleY = vpH * kInvRefH;
            break;
        }
        case 1:
        case 2: {
            const double uScale = std::min(safeW * kInvRefW, safeH * kInvRefH);
            io->safeL = sL;
            io->safeT = sT;
            io->safeR = sR;
            io->safeB = sB;
            io->cachedW = safeW;
            io->cachedH = safeH;
            io->scaleX = uScale;
            io->scaleY = uScale;
            break;
        }
        case 3:
        case 4: {
            const double uScale = std::min(vpW * kInvRefW, vpH * kInvRefH);
            io->safeL = 0.0;
            io->safeT = 0.0;
            io->safeR = vpW;
            io->safeB = vpH;
            io->cachedW = vpW;
            io->cachedH = vpH;
            io->scaleX = uScale;
            io->scaleY = uScale;
            break;
        }
        default: {
            io->safeL = sL;
            io->safeT = sT;
            io->safeR = sR;
            io->safeB = sB;
            io->cachedW = vpW;
            io->cachedH = vpH;
            io->scaleX = 1.0;
            io->scaleY = 1.0;
            break;
        }
    }
}

extern "C" bool __fastcall NormalizeViewportStruct(
    uint8_t* vpInfo,
    uint32_t* outW,
    uint32_t* outH,
    float* outAspect) {
    if (!IsValidUserPtr(vpInfo)) {
        return false;
    }
    const uint32_t w = *reinterpret_cast<const uint32_t*>(vpInfo + 0x08);
    const uint32_t h = *reinterpret_cast<const uint32_t*>(vpInfo + 0x0c);
    if (w < 200 || h < 200 || w > 16384 || h > 16384) {
        return false;
    }

    const float exactAspect = static_cast<float>(w) / static_cast<float>(h);

    // Disable 2.2:1 ultrawide 3-monitor split & MultiMonBlackBars trigger
    *(vpInfo + 0x03) = 0;
    *reinterpret_cast<float*>(vpInfo + 0x10) = exactAspect;
    *reinterpret_cast<uint32_t*>(vpInfo + 0x20) = 1;

    auto* rects = *reinterpret_cast<int32_t**>(vpInfo + 0x18);
    if (IsValidUserPtr(rects)) {
        rects[0] = 0;
        rects[1] = 0;
        rects[2] = static_cast<int32_t>(w) - 1;
        rects[3] = static_cast<int32_t>(h) - 1;
    }

    if (outW) *outW = w;
    if (outH) *outH = h;
    if (outAspect) *outAspect = exactAspect;
    return true;
}

int InitializeEngineSymbols(HMODULE hSelf, HMODULE hFC64) {
    if (!hFC64) {
        return 0;
    }
    HookEngine::LogInit(hSelf, L"FC4_AspectFix.log");

    if (InterlockedCompareExchange(&g_SymbolsResolved, 1, 0) != 0) {
        return g_Sym.ready ? 10 : 0;
    }

    LoadPatchConfig(hSelf, g_Config);
    HookEngine::LogMsg(
        "[CONFIG] Enabled=%d DisableBlackBars=%d DisableCinematicBlackBars=%d "
        "AdaptiveUI=%d FixFOV=%d UnlockResolutions=%d",
        g_Config.enabled ? 1 : 0,
        g_Config.disableBlackBars ? 1 : 0,
        g_Config.disableCinematicBlackBars ? 1 : 0,
        g_Config.adaptiveUi ? 1 : 0,
        g_Config.fixFov ? 1 : 0,
        g_Config.unlockResolutions ? 1 : 0);

    if (!g_Config.enabled) {
        HookEngine::LogMsg("[INFO] Disabled via FC4_AspectFix.ini");
        return 0;
    }

    HookEngine::LogMsg("[INFO] FC64.dll base=0x%p (read-only AOB discovery; zero .text writes)", hFC64);

    g_Sym = EngineSymbols{};
    g_Sym.hFC64 = hFC64;
    int resolved = 0;
    uint32_t matches = 0;

    // 1. g_DisplayManagerPtr (RVA 0x2dd84c8, from ContainerScale 0x1301690)
    if (const uint8_t* p = HookEngine::FindPatternInModule(
            hFC64,
            "Sym1_DisplayManager",
            "48 89 5c 24 08 57 48 83 ec 30 48 8b f9 48 8b 0d ?? ?? ?? ?? e8 ?? ?? ?? ?? "
            "48 8b 4f 08 33 d2 48 8b 49 18 48 8b d8 e8 ?? ?? ?? ?? 8b 53 20 48 8b 4f 10",
            0x1301690,
            matches);
        p && matches == 1) {
        g_Sym.displayManagerPtr = reinterpret_cast<uint8_t**>(
            const_cast<uint8_t*>(HookEngine::ResolveRipRelative(p + 20, ReadI32(p + 16))));
        HookEngine::LogMsg("[OK] Sym1_DisplayManager -> RVA 0x%llX", RvaOf(hFC64, g_Sym.displayManagerPtr));
        ++resolved;
    }

    // 2. g_RenderConfigPtr (RVA 0x2dd7e08)
    if (const uint8_t* p = HookEngine::FindPatternInModule(
            hFC64,
            "Sym2_RenderConfig",
            "48 83 ec 48 48 8b 0d ?? ?? ?? ?? 0f 29 74 24 30 0f 29 7c 24 20 0f 28 f9 0f 28 f0 83 79 50 00",
            0x234340,
            matches);
        p && matches == 1) {
        g_Sym.renderConfigPtr = reinterpret_cast<uint8_t**>(
            const_cast<uint8_t*>(HookEngine::ResolveRipRelative(p + 11, ReadI32(p + 7))));
        HookEngine::LogMsg("[OK] Sym2_RenderConfig -> RVA 0x%llX", RvaOf(hFC64, g_Sym.renderConfigPtr));
        ++resolved;
    }

    // 3. g_ModeProviderPtr (RVA 0x2dd8538) + OptionsFilter return address (RVA 0xfaab36)
    if (const uint8_t* p = HookEngine::FindPatternInModule(
            hFC64,
            "Sym3_ModeProvider",
            "48 8b 0d ?? ?? ?? ?? 45 33 f6 48 85 c9 0f 84 8c 00 00 00 48 8b 01 "
            "49 8d 9f a0 00 00 00 45 33 c0 48 8b d3 ff 50 10",
            0xfaab10,
            matches);
        p && matches == 1) {
        g_Sym.modeProviderPtr = reinterpret_cast<void**>(
            const_cast<uint8_t*>(HookEngine::ResolveRipRelative(p + 7, ReadI32(p + 3))));
        g_Sym.optionsFilterRetAddr = p + 38;
        HookEngine::LogMsg("[OK] Sym3_ModeProvider -> RVA 0x%llX (ret RVA 0x%llX)",
                           RvaOf(hFC64, g_Sym.modeProviderPtr),
                           RvaOf(hFC64, g_Sym.optionsFilterRetAddr));
        ++resolved;
    }

    // 4. g_ModeProviderVTableExpected (RVA 0x26d4b10)
    if (const uint8_t* p = HookEngine::FindPatternInModule(
            hFC64,
            "Sym4_ModeProviderVTable",
            "48 83 ec 28 33 d2 8d 4a 60 e8 ?? ?? ?? ?? 48 8d 0d ?? ?? ?? ?? 48 89 08 33 c9 48 89 48 08",
            0x18a120,
            matches);
        p && matches == 1) {
        g_Sym.modeProviderVTableExp = HookEngine::ResolveRipRelative(p + 21, ReadI32(p + 17));
        HookEngine::LogMsg("[OK] Sym4_ModeProviderVTable -> RVA 0x%llX",
                           RvaOf(hFC64, g_Sym.modeProviderVTableExp));
        ++resolved;
    }

    // 5. g_UiMovieMgrPtr (RVA 0x2e22980) + fn_TreeNext (RVA 0x1d822e0)
    if (const uint8_t* p = HookEngine::FindPatternInModule(
            hFC64,
            "Sym5_UiMovieMgr",
            "48 8b 3d ?? ?? ?? ?? 48 8d 15 ?? ?? ?? ?? 48 8d 4c 24 48 e8 ?? ?? ?? ?? "
            "48 8b cf 48 8b d0 e8 ?? ?? ?? ?? 48 85 c0 74 10 48 8b 48 18 48 85 c9 74 07 "
            "33 d2 e8 ?? ?? ?? ?? 48 8b cb e8 ?? ?? ?? ?? 48 8b d8 48 3b c6 75 a8",
            0x762dd0,
            matches);
        p && matches == 1) {
        g_Sym.uiMovieMgrPtr = reinterpret_cast<uint8_t**>(
            const_cast<uint8_t*>(HookEngine::ResolveRipRelative(p + 7, ReadI32(p + 3))));
        g_Sym.fnTreeNext = reinterpret_cast<FnTreeNext>(
            const_cast<uint8_t*>(HookEngine::ResolveRipRelative(p + 64, ReadI32(p + 60))));
        HookEngine::LogMsg("[OK] Sym5_UiMovieMgr -> RVA 0x%llX (TreeNext RVA 0x%llX)",
                           RvaOf(hFC64, g_Sym.uiMovieMgrPtr),
                           RvaOf(hFC64, reinterpret_cast<const void*>(g_Sym.fnTreeNext)));
        ++resolved;
    }

    // 6. g_SafeFrameFloats (RVA 0x2d2b94c)
    if (const uint8_t* p = HookEngine::FindPatternInModule(
            hFC64,
            "Sym6_SafeFrame",
            "48 83 ec 28 f3 0f 10 25 ?? ?? ?? ?? 0f 28 d4 f3 0f 5c e1 f3 0f 5c d0 "
            "f3 0f 59 25 ?? ?? ?? ?? f3 0f 59 15 ?? ?? ?? ?? f3 0f 11 25 ?? ?? ?? ?? "
            "f3 0f 58 e1 f3 0f 11 15 ?? ?? ?? ??",
            0x762d30,
            matches);
        p && matches == 1) {
        g_Sym.safeFrameFloats = reinterpret_cast<const float*>(
            HookEngine::ResolveRipRelative(p + 59, ReadI32(p + 55)));
        HookEngine::LogMsg("[OK] Sym6_SafeFrame -> RVA 0x%llX", RvaOf(hFC64, g_Sym.safeFrameFloats));
        ++resolved;
    }

    // 7. Scaleform IsValid (RVA 0x1cb8fa0)
    if (const uint8_t* p = HookEngine::FindPatternInModule(
            hFC64,
            "Sym7_ScaleformIsValid",
            "48 8b 77 28 48 85 f6 74 45 48 8b 4e 08 48 85 c9 74 24 48 8b 49 10 e8 ?? ?? ?? ?? 84 c0 74 17",
            0x761af4,
            matches);
        p && matches == 1) {
        g_Sym.fnIsValid = reinterpret_cast<FnIsValid>(
            const_cast<uint8_t*>(HookEngine::ResolveRipRelative(p + 27, ReadI32(p + 23))));
        HookEngine::LogMsg("[OK] Sym7_ScaleformIsValid -> RVA 0x%llX",
                           RvaOf(hFC64, reinterpret_cast<const void*>(g_Sym.fnIsValid)));
        ++resolved;
    }

    // 8. Scaleform SetXScale / SetYScale (RVA 0x1cb92f0 / 0x1cb93b0)
    if (const uint8_t* p = HookEngine::FindPatternInModule(
            hFC64,
            "Sym8_ScaleformSetScale",
            "49 8b 4a 10 48 8d 54 24 20 66 0f 28 d6 e8 ?? ?? ?? ?? 8b 08 89 8c 24 10 01 00 00 "
            "48 8d 8c 24 10 01 00 00 e8 ?? ?? ?? ?? 8b 43 34 83 f8 01 74 05",
            0x74a0d1,
            matches);
        p && matches == 1 && p[0x47] == 0xE8) {
        g_Sym.fnSetXScale = reinterpret_cast<FnSetProp>(
            const_cast<uint8_t*>(HookEngine::ResolveRipRelative(p + 18, ReadI32(p + 14))));
        g_Sym.fnSetYScale = reinterpret_cast<FnSetProp>(
            const_cast<uint8_t*>(HookEngine::ResolveRipRelative(p + 0x4C, ReadI32(p + 0x48))));
        HookEngine::LogMsg("[OK] Sym8_ScaleformSetScale -> X=0x%llX Y=0x%llX",
                           RvaOf(hFC64, reinterpret_cast<const void*>(g_Sym.fnSetXScale)),
                           RvaOf(hFC64, reinterpret_cast<const void*>(g_Sym.fnSetYScale)));
        ++resolved;
    }

    // 9. Scaleform SetX / SetY (RVA 0x1cb9090 / 0x1cb9150)
    if (const uint8_t* p = HookEngine::FindPatternInModule(
            hFC64,
            "Sym9_ScaleformSetPos",
            "48 8b 4b 08 48 8d 54 24 20 66 41 0f 28 d0 48 8b 49 10 e8 ?? ?? ?? ?? "
            "8b 08 89 8c 24 10 01 00 00 48 8d 8c 24 10 01 00 00 e8 ?? ?? ?? ?? "
            "48 8b 4b 08 48 8d 54 24 20 66 0f 28 d7 48 8b 49 10 e8 ?? ?? ?? ??",
            0x74a222,
            matches);
        p && matches == 1) {
        g_Sym.fnSetX = reinterpret_cast<FnSetProp>(
            const_cast<uint8_t*>(HookEngine::ResolveRipRelative(p + 23, ReadI32(p + 19))));
        g_Sym.fnSetY = reinterpret_cast<FnSetProp>(
            const_cast<uint8_t*>(HookEngine::ResolveRipRelative(p + 67, ReadI32(p + 63))));
        HookEngine::LogMsg("[OK] Sym9_ScaleformSetPos -> X=0x%llX Y=0x%llX",
                           RvaOf(hFC64, reinterpret_cast<const void*>(g_Sym.fnSetX)),
                           RvaOf(hFC64, reinterpret_cast<const void*>(g_Sym.fnSetY)));
        ++resolved;
    }

    // 10. BlackBars service lookup + vtable method targets
    const uint8_t* pUiSys = HookEngine::FindPatternInModule(
        hFC64,
        "Sym10A_UiSystemBlackBars",
        "48 89 5c 24 10 48 89 6c 24 20 56 48 83 ec 20 0f b6 e9 48 8b 0d ?? ?? ?? ?? "
        "8b f2 48 8d 54 24 40 45 33 c0 e8 ?? ?? ?? ?? 48 8b 4c 24 40 48 8b 59 10 48 85 db 74 63 "
        "48 89 7c 24 30 e8 ?? ?? ?? ?? 48 8b cb 48 8d 50 1c e8 ?? ?? ?? ??",
        0x19127c0,
        matches);
    uint32_t mSetVis = 0, mFadeIn = 0;
    const uint8_t* pSetVis = HookEngine::FindPatternInModule(
        hFC64,
        "Sym10B_BlackBarsSetVis",
        "48 89 5c 24 10 57 48 83 ec 20 48 8b d9 48 8b 89 a8 01 00 00 0f b6 fa 48 85 c9 74 36",
        0x74a990,
        mSetVis);
    const uint8_t* pFadeIn = HookEngine::FindPatternInModule(
        hFC64,
        "Sym10C_BlackBarsFadeIn",
        "40 53 48 83 ec 20 80 b9 22 01 00 00 00 48 8b d9 74 4f "
        "80 b9 21 01 00 00 00 74 46 80 b9 20 01 00 00 00 74 3d "
        "48 8b 89 88 01 00 00 48 85 c9 74 31 48 8b 49 10 4c 8d 05 ?? ?? ?? ?? "
        "48 8d 54 24 38 41 b1 01 e8 ?? ?? ?? ?? 8b 08 89 4c 24 30 "
        "48 8d 4c 24 30 e8 ?? ?? ?? ?? 66 c7 83 e9 00 00 00 01 01",
        0x74a7d0,
        mFadeIn);
    if (pUiSys && matches == 1 && pSetVis && mSetVis == 1 && pFadeIn && mFadeIn == 1) {
        g_Sym.uiSystemPtr = reinterpret_cast<uint8_t**>(
            const_cast<uint8_t*>(HookEngine::ResolveRipRelative(pUiSys + 25, ReadI32(pUiSys + 21))));
        g_Sym.fnGetUi = reinterpret_cast<FnGetUi>(
            const_cast<uint8_t*>(HookEngine::ResolveRipRelative(pUiSys + 40, ReadI32(pUiSys + 36))));
        g_Sym.fnGetBlackBarsId = reinterpret_cast<FnGetBlackBarsId>(
            const_cast<uint8_t*>(HookEngine::ResolveRipRelative(pUiSys + 64, ReadI32(pUiSys + 60))));
        g_Sym.fnLookupService = reinterpret_cast<FnLookupService>(
            const_cast<uint8_t*>(HookEngine::ResolveRipRelative(pUiSys + 76, ReadI32(pUiSys + 72))));
        g_Sym.blackBarsSetVisAddr = pSetVis;
        g_Sym.blackBarsFadeInAddr = pFadeIn;
        HookEngine::LogMsg("[OK] Sym10_BlackBars -> UiSys RVA 0x%llX SetVis=0x%llX FadeIn=0x%llX",
                           RvaOf(hFC64, g_Sym.uiSystemPtr),
                           RvaOf(hFC64, pSetVis),
                           RvaOf(hFC64, pFadeIn));
        ++resolved;
    }

    g_Sym.ready = (resolved == 10);
    HookEngine::LogMsg("[SUMMARY] Resolved %d/10 engine symbols (zero .text bytes modified)", resolved);
    return resolved;
}

void TickAdaptiveEngineOnMainThread() {
    if (!g_Sym.ready || !g_Config.enabled) {
        return;
    }

    uint32_t vpW = 0, vpH = 0;
    float aspect = 0.0f;
    if (NormalizeDisplayAndRenderConfig(vpW, vpH, aspect)) {
        ApplyAdaptiveUiAnchors(static_cast<double>(vpW), static_cast<double>(vpH));
    }
    TryInstallModeProviderHook();
    TryInstallBlackBarsHook();
}

void StartAdaptiveController(HMODULE hSelf, HMODULE hFC64) {
    if (InitializeEngineSymbols(hSelf, hFC64) != 10) {
        return;
    }
    if (InterlockedCompareExchange(&g_ControllerStarted, 1, 0) != 0) {
        return;
    }
    g_WatcherThread = CreateThread(nullptr, 0, WindowAttachThreadProc, nullptr, 0, nullptr);
    if (g_WatcherThread) {
        HookEngine::LogMsg("[INFO] Adaptive window watcher started");
    }
}

void ShutdownAdaptiveController() {
    InterlockedExchange(&g_ShuttingDown, 1);
    if (g_WatcherThread) {
        WaitForSingleObject(g_WatcherThread, 500);
        CloseHandle(g_WatcherThread);
        g_WatcherThread = nullptr;
    }
}
