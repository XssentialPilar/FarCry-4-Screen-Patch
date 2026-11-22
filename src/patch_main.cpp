#include "patch_main.h"
#include "hook_engine.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <cwctype>
#include <unordered_map>

namespace {

constexpr UINT_PTR kAdaptiveTimerId = 0xFC41;
constexpr UINT kAdaptiveTimerMs = 33;
constexpr UINT kMsgStartTimer = WM_APP + 0x41;
constexpr double kAppliedMarkerEps = 1e-4;
constexpr DWORD kVideoSkipGraceMs = 350;
constexpr DWORD kToggleToastMs = 2500;

using FnTreeNext = void*(__fastcall*)(void* node);
using FnIsValid = bool(__fastcall*)(void* gfxObj);
using FnSetProp = uint32_t*(__fastcall*)(void* gfxObj, uint32_t* outStatus, double val);
using FnSetVisible = uint32_t*(__fastcall*)(void* gfxObj, uint32_t* outStatus, uint8_t visible);
using FnSetText = uint32_t*(__fastcall*)(void* gfxObj, uint32_t* outStatus, const wchar_t* text);
using FnSpriteNodeSetVis = void(__fastcall*)(void* spriteNode, uint8_t visible);
using FnSpriteNodeInval = uint8_t(__fastcall*)(void* spriteNode);
using FnGetUi = void(__fastcall*)(void* uiSystem, void** outHolder, uint32_t idx);
using FnGetBlackBarsId = uint8_t*(__fastcall*)(void);
using FnLookupService = uint8_t*(__fastcall*)(void* uiMgr, void* idPtr);
using FnGetSupportedModes = void(__fastcall*)(void* thisPtr, void* outVec, uint32_t flags);
using FnStaticBlackBarsSetVis = void(__fastcall*)(void* thisPtr, uint8_t visible);
using FnBlackBarsFadeIn = void(__fastcall*)(void* thisPtr);
using FnBlackBarsFadeOut = void(__fastcall*)(void* thisPtr);

// Dynamically loaded XInputGetState (zero static XInput DLL imports)
struct XInputGamepad {
    WORD wButtons;
    BYTE bLeftTrigger;
    BYTE bRightTrigger;
    SHORT sThumbLX;
    SHORT sThumbLY;
    SHORT sThumbRX;
    SHORT sThumbRY;
};
struct XInputState {
    DWORD dwPacketNumber;
    XInputGamepad Gamepad;
};
using FnXInputGetState = DWORD(WINAPI*)(DWORD dwUserIndex, XInputState* pState);

struct Mesh2DVertex {
    float x;
    float y;
    float z;
    uint32_t color;
    float u;
    float v;
};
static_assert(sizeof(Mesh2DVertex) == 24, "Mesh2DVertex must be 24 bytes");

struct EngineSymbols {
    HMODULE hFC64 = nullptr;
    uint8_t** displayManagerPtr = nullptr;        // RVA 0x2dd84c8
    uint8_t** renderConfigPtr = nullptr;          // RVA 0x2dd7e08
    void** modeProviderPtr = nullptr;             // RVA 0x2dd8538
    const void* modeProviderVTableExp = nullptr;  // RVA 0x26d4b10
    const void* optionsFilterRetAddr = nullptr;   // RVA 0xfaab36
    uint8_t** uiMovieMgrPtr = nullptr;            // RVA 0x2e22980
    const float* safeFrameFloats = nullptr;       // RVA 0x2d2b94c [L, R, T, B]
    uint8_t** uiSystemPtr = nullptr;              // RVA 0x2e24c58
    FnGetUi fnGetUi = nullptr;                    // RVA 0x83e3c0
    FnGetBlackBarsId fnGetBlackBarsId = nullptr;  // RVA 0x87dc80
    FnLookupService fnLookupService = nullptr;    // RVA 0x7248e0
    const void* blackBarsSetVisAddr = nullptr;    // RVA 0x74a990
    const void* blackBarsFadeInAddr = nullptr;    // RVA 0x74a7d0
    FnTreeNext fnTreeNext = nullptr;              // RVA 0x1d822e0
    FnIsValid fnIsValid = nullptr;                // RVA 0x1cb8fa0
    FnSetProp fnSetXScale = nullptr;              // RVA 0x1cb92f0
    FnSetProp fnSetYScale = nullptr;              // RVA 0x1cb93b0
    FnSetProp fnSetX = nullptr;                   // RVA 0x1cb9090
    FnSetProp fnSetY = nullptr;                   // RVA 0x1cb9150
    FnSetVisible fnSetVisible = nullptr;          // RVA 0x1cb9550
    FnSetText fnSetText = nullptr;                // RVA 0x1cb99f0
    FnSpriteNodeSetVis fnSpriteNodeSetVis = nullptr;   // RVA 0x1d32b00
    FnSpriteNodeInval fnSpriteNodeInval = nullptr;     // RVA 0x1d2c180
    uint8_t** mesh2DPoolHeaderPtr = nullptr;      // RVA 0x2d1e438
    uint8_t** video2DArrayHeaderPtr = nullptr;    // RVA 0x2d1dc98
    uint8_t** binkPlayerMgrPtr = nullptr;         // RVA 0x2e4c4a8
    const void* binkResourceVTable = nullptr;     // RVA 0x26d50f0
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
    uint8_t specialKind = 0;
};

PatchConfig g_Config{};
EngineSymbols g_Sym{};
wchar_t g_IniPath[MAX_PATH]{};
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

// Pre-recorded Bink video skip & discrete native prompt state
void* g_PromptGfxObj = nullptr;  // SubTitleText2 in fireuiplayer
bool g_PromptOwnedByPatch = false;
wchar_t g_LastPromptText[128]{};
uint8_t* g_ActiveSkippableBinkRes = nullptr;
DWORD g_ActiveBinkStartTick = 0;
volatile LONG g_PendingSkipRequest = 0;
DWORD g_ToggleToastUntilTick = 0;
wchar_t g_ToggleToastText[128]{};
FnXInputGetState g_FnXInputGetState = nullptr;
bool g_XInputProbed = false;
WORD g_PrevPadButtons[4]{};

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

bool ContainsCaseInsensitiveW(const wchar_t* haystack, const wchar_t* needle) {
    if (!haystack || !needle || !*needle) {
        return false;
    }
    const size_t nlen = wcslen(needle);
    for (size_t i = 0; i < 96 && haystack[i] != 0; ++i) {
        size_t j = 0;
        while (j < nlen && haystack[i + j] != 0 &&
               towlower(haystack[i + j]) == towlower(needle[j])) {
            ++j;
        }
        if (j == nlen) {
            return true;
        }
    }
    return false;
}

bool EqualsCaseInsensitiveW(const wchar_t* a, const wchar_t* b) {
    if (!a || !b) {
        return false;
    }
    size_t i = 0;
    while (i < 96 && a[i] != 0 && b[i] != 0) {
        if (towlower(a[i]) != towlower(b[i])) {
            return false;
        }
        ++i;
    }
    return a[i] == 0 && b[i] == 0;
}

bool StartsWithCaseInsensitiveW(const wchar_t* s, const wchar_t* prefix) {
    if (!s || !prefix) {
        return false;
    }
    for (size_t i = 0; prefix[i] != 0; ++i) {
        if (s[i] == 0 || towlower(s[i]) != towlower(prefix[i])) {
            return false;
        }
    }
    return true;
}

bool ReadAnchorNameSafe(const void* node, wchar_t* outBuf, size_t maxChars) {
    outBuf[0] = L'\0';
    auto* strObj = *reinterpret_cast<uint8_t* const*>(reinterpret_cast<const uint8_t*>(node) + 32);
    if (!IsValidUserPtr(strObj)) {
        return false;
    }
    const wchar_t* wname = reinterpret_cast<const wchar_t*>(strObj + 24);
    size_t i = 0;
    while (i + 1 < maxChars && i < 92 && wname[i] != 0) {
        outBuf[i] = wname[i];
        ++i;
    }
    outBuf[i] = L'\0';
    return i > 0;
}

void EnsureXInputLoaded() {
    if (g_XInputProbed) {
        return;
    }
    g_XInputProbed = true;
    const wchar_t* dlls[] = {L"xinput1_4.dll", L"xinput1_3.dll", L"xinput9_1_0.dll"};
    for (const wchar_t* name : dlls) {
        HMODULE h = GetModuleHandleW(name);
        if (!h) {
            h = LoadLibraryW(name);
        }
        if (h) {
            g_FnXInputGetState = reinterpret_cast<FnXInputGetState>(GetProcAddress(h, "XInputGetState"));
            if (g_FnXInputGetState) {
                return;
            }
        }
    }
}

void UpdateDiscretePrompt(const wchar_t* text) {
    if (!g_Sym.fnIsValid || !g_Sym.fnSetText || !g_Sym.fnSetVisible) {
        return;
    }
    if (!IsValidUserPtr(g_PromptGfxObj) || !g_Sym.fnIsValid(g_PromptGfxObj)) {
        g_PromptGfxObj = nullptr;
        g_PromptOwnedByPatch = false;
        g_LastPromptText[0] = L'\0';
        return;
    }

    if (text && text[0] != L'\0') {
        if (!g_PromptOwnedByPatch || wcscmp(g_LastPromptText, text) != 0) {
            uint32_t status = 0;
            g_Sym.fnSetText(g_PromptGfxObj, &status, text);
            g_Sym.fnSetVisible(g_PromptGfxObj, &status, 1);
            wcsncpy_s(g_LastPromptText, 128, text, _TRUNCATE);
            g_PromptOwnedByPatch = true;
        }
    } else if (g_PromptOwnedByPatch) {
        uint32_t status = 0;
        g_Sym.fnSetText(g_PromptGfxObj, &status, L"");
        g_Sym.fnSetVisible(g_PromptGfxObj, &status, 0);
        g_LastPromptText[0] = L'\0';
        g_PromptOwnedByPatch = false;
    }
}

void TogglePreRecordedVideoSkipInGame() {
    g_Config.skipPreRecordedVideos = !g_Config.skipPreRecordedVideos;
    if (g_IniPath[0] != L'\0') {
        WritePrivateProfileStringW(
            L"Video",
            L"SkipPreRecordedVideos",
            g_Config.skipPreRecordedVideos ? L"1" : L"0",
            g_IniPath);
    }
    _snwprintf_s(
        g_ToggleToastText,
        128,
        _TRUNCATE,
        g_Config.skipPreRecordedVideos
            ? L"Video Skip: ON  (Press any key during videos)  [F8]"
            : L"Video Skip: OFF  [F8]");
    g_ToggleToastUntilTick = GetTickCount() + kToggleToastMs;
    HookEngine::LogMsg("[LIVE] User toggled SkipPreRecordedVideos=%d via F8",
                       g_Config.skipPreRecordedVideos ? 1 : 0);
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

// Convert any letterboxed/pillarboxed Bink video quad in Mesh2D pool (0x2d1e438)
// to full-screen Cover-Fit with 1:1 square pixels (zero black bars, zero stretching).
void FixBinkVideoQuads() {
    if (!g_Config.fixVideoAspectRatio) {
        return;
    }
    if (g_Sym.mesh2DPoolHeaderPtr && IsValidUserPtr(*g_Sym.mesh2DPoolHeaderPtr)) {
        uint8_t* poolHdr = *g_Sym.mesh2DPoolHeaderPtr;
        void** items = *reinterpret_cast<void***>(poolHdr);
        const uint32_t cap = *reinterpret_cast<const uint32_t*>(poolHdr + 8);
        if (IsValidUserPtr(items) && cap <= 4096) {
            for (uint32_t i = 0; i < cap; ++i) {
                auto* mesh = reinterpret_cast<uint8_t*>(items[i]);
                if (!IsValidUserPtr(mesh)) {
                    continue;
                }
                const uint32_t vCount = *reinterpret_cast<const uint32_t*>(mesh + 16);
                if (vCount != 4) {
                    continue;
                }
                const float sortKey = *reinterpret_cast<const float*>(mesh + 76);
                if (std::fabs(sortKey - 1.0f) > 1e-3f) {
                    continue;
                }
                auto* v = *reinterpret_cast<Mesh2DVertex**>(mesh + 8);
                if (!IsValidUserPtr(v)) {
                    continue;
                }
                // Match exact BinkQuadSetup (0x1532c90) vertex layout:
                // v[0]=(x0,y1,1) uv=(0,1), v[1]=(x1,y1,1) uv=(1,1),
                // v[2]=(x1,y0,1) uv=(1,0), v[3]=(x0,y0,1) uv=(0,0)
                if (std::fabs(v[0].z - 1.0f) > 1e-3f || std::fabs(v[1].z - 1.0f) > 1e-3f ||
                    std::fabs(v[2].z - 1.0f) > 1e-3f || std::fabs(v[3].z - 1.0f) > 1e-3f) {
                    continue;
                }
                if (std::fabs(v[0].u - 0.0f) > 1e-3f || std::fabs(v[0].v - 1.0f) > 1e-3f ||
                    std::fabs(v[1].u - 1.0f) > 1e-3f || std::fabs(v[1].v - 1.0f) > 1e-3f ||
                    std::fabs(v[2].u - 1.0f) > 1e-3f || std::fabs(v[2].v - 0.0f) > 1e-3f ||
                    std::fabs(v[3].u - 0.0f) > 1e-3f || std::fabs(v[3].v - 0.0f) > 1e-3f) {
                    continue;
                }
                const float x0 = v[0].x;
                const float x1 = v[1].x;
                const float y1 = v[0].y;
                const float y0 = v[2].y;
                if (std::fabs(v[3].x - x0) > 1e-3f || std::fabs(v[2].x - x1) > 1e-3f ||
                    std::fabs(v[1].y - y1) > 1e-3f || std::fabs(v[3].y - y0) > 1e-3f) {
                    continue;
                }
                if (x1 <= x0 + 0.05f || y1 <= y0 + 0.05f) {
                    continue;
                }

                const bool isLetterboxed =
                    std::fabs(x0) < 1e-3f && std::fabs(x1 - 1.0f) < 1e-3f &&
                    y0 > 0.002f && y1 < 0.998f && std::fabs((y0 + y1) - 1.0f) < 0.02f;
                const bool isPillarboxed =
                    std::fabs(y0) < 1e-3f && std::fabs(y1 - 1.0f) < 1e-3f &&
                    x0 > 0.002f && x1 < 0.998f && std::fabs((x0 + x1) - 1.0f) < 0.02f;

                if (!isLetterboxed && !isPillarboxed) {
                    continue;
                }

                float u0 = 0.0f, u1 = 1.0f, v0 = 0.0f, v1 = 1.0f;
                if (isLetterboxed) {
                    const float uSpan = std::clamp(y1 - y0, 0.05f, 1.0f);
                    u0 = 0.5f * (1.0f - uSpan);
                    u1 = 1.0f - u0;
                } else {
                    const float vSpan = std::clamp(x1 - x0, 0.05f, 1.0f);
                    v0 = 0.5f * (1.0f - vSpan);
                    v1 = 1.0f - v0;
                }

                v[0].x = 0.0f; v[0].y = 1.0f; v[0].u = u0; v[0].v = v1;
                v[1].x = 1.0f; v[1].y = 1.0f; v[1].u = u1; v[1].v = v1;
                v[2].x = 1.0f; v[2].y = 0.0f; v[2].u = u1; v[2].v = v0;
                v[3].x = 0.0f; v[3].y = 0.0f; v[3].u = u0; v[3].v = v0;

                HookEngine::LogMsg(
                    "[LIVE] Bink video Cover-Fit applied (orig=[%.3f,%.3f,%.3f,%.3f] -> uv=[%.3f..%.3f, %.3f..%.3f])",
                    x0, y0, x1, y1, u0, u1, v0, v1);
            }
        }
    }

    // Secondary 2D video quad pool (0x2d1dc98): clear legacy 16:9 squeeze flag (+0x55) if active
    if (g_Sym.video2DArrayHeaderPtr && IsValidUserPtr(*g_Sym.video2DArrayHeaderPtr)) {
        uint8_t* poolHdr = *g_Sym.video2DArrayHeaderPtr;
        void** items = *reinterpret_cast<void***>(poolHdr);
        const uint32_t cap = *reinterpret_cast<const uint32_t*>(poolHdr + 8);
        if (IsValidUserPtr(items) && cap <= 4096) {
            for (uint32_t i = 0; i < cap; ++i) {
                auto* q = reinterpret_cast<uint8_t*>(items[i]);
                if (!IsValidUserPtr(q)) {
                    continue;
                }
                if (*(q + 0x10) != 0 && *(q + 0x55) != 0) {
                    *(q + 0x55) = 0;
                    static volatile LONG s_loggedV2D = 0;
                    if (InterlockedCompareExchange(&s_loggedV2D, 1, 0) == 0) {
                        HookEngine::LogMsg("[LIVE] Video2D legacy 16:9 aspect squeeze disabled");
                    }
                }
            }
        }
    }
}

bool CheckSkippableBinkResource(
    uint8_t* binkRes,
    uint8_t*& outHbink,
    uint32_t& outFrames,
    uint32_t& outFrameNum) {
    if (!IsValidUserPtr(binkRes)) {
        return false;
    }
    if (g_Sym.binkResourceVTable &&
        *reinterpret_cast<const void* const*>(binkRes) != g_Sym.binkResourceVTable) {
        return false;
    }
    uint8_t* hbink = *reinterpret_cast<uint8_t**>(binkRes + 32);
    const uint8_t isLooping = *(binkRes + 51);
    if (!IsValidUserPtr(hbink) || isLooping != 0) {
        return false;
    }
    const uint32_t openFlags = *reinterpret_cast<const uint32_t*>(hbink + 0x20);
    if ((openFlags & 0x80U) != 0) {  // BINKWILLLOOP
        return false;
    }
    const uint32_t fTotal = *reinterpret_cast<const uint32_t*>(hbink + 8);
    const uint32_t fCur = *reinterpret_cast<const uint32_t*>(hbink + 12);
    if (fTotal <= 1 || fTotal >= 1000000 || fCur >= fTotal) {
        return false;
    }
    outHbink = hbink;
    outFrames = fTotal;
    outFrameNum = fCur;
    return true;
}

// Detect active non-looping pre-recorded Bink video (both in-game cutscenes via BinkPlayerMgr
// and startup intro videos via Mesh2DPool), show discrete prompt, and skip on any button/key.
void UpdatePreRecordedVideoSkip() {
    const DWORD now = GetTickCount();

    uint8_t* activeBinkRes = nullptr;
    uint8_t* activeHbink = nullptr;
    uint32_t frames = 0;
    uint32_t frameNum = 0;

    // Source 1: BinkPlayerMgr (in-game story cutscenes / mission videos)
    if (g_Sym.binkPlayerMgrPtr && IsValidUserPtr(*g_Sym.binkPlayerMgrPtr)) {
        uint8_t* mgr = *g_Sym.binkPlayerMgrPtr;
        uint8_t* binkPlayer = *reinterpret_cast<uint8_t**>(mgr + 8);
        if (IsValidUserPtr(binkPlayer)) {
            uint8_t* binkRes = *reinterpret_cast<uint8_t**>(binkPlayer + 16);
            if (CheckSkippableBinkResource(binkRes, activeHbink, frames, frameNum)) {
                activeBinkRes = binkRes;
            }
        }
    }

    // Source 2: Mesh2DPool (startup intro videos via Initial Screens & fullscreen Bink quads)
    if (!activeBinkRes && g_Sym.mesh2DPoolHeaderPtr && IsValidUserPtr(*g_Sym.mesh2DPoolHeaderPtr)) {
        uint8_t* poolHdr = *g_Sym.mesh2DPoolHeaderPtr;
        void** items = *reinterpret_cast<void***>(poolHdr);
        const uint32_t cap = *reinterpret_cast<const uint32_t*>(poolHdr + 8);
        if (IsValidUserPtr(items) && cap <= 4096) {
            for (uint32_t i = 0; i < cap; ++i) {
                auto* mesh = reinterpret_cast<uint8_t*>(items[i]);
                if (!IsValidUserPtr(mesh)) {
                    continue;
                }
                if (*(mesh + 81) == 0) {  // not visible
                    continue;
                }
                const uint32_t vCount = *reinterpret_cast<const uint32_t*>(mesh + 16);
                const uint32_t gCount = *reinterpret_cast<const uint32_t*>(mesh + 48);
                if (vCount != 4 || gCount == 0 || gCount > 16) {
                    continue;
                }
                uint8_t* groups = *reinterpret_cast<uint8_t**>(mesh + 40);
                if (!IsValidUserPtr(groups)) {
                    continue;
                }
                uint8_t* binkRes = *reinterpret_cast<uint8_t**>(groups + 40);
                if (CheckSkippableBinkResource(binkRes, activeHbink, frames, frameNum)) {
                    activeBinkRes = binkRes;
                    break;
                }
            }
        }
    }

    if (activeBinkRes != g_ActiveSkippableBinkRes) {
        g_ActiveSkippableBinkRes = activeBinkRes;
        g_ActiveBinkStartTick = now;
        InterlockedExchange(&g_PendingSkipRequest, 0);
        if (activeBinkRes) {
            HookEngine::LogMsg("[LIVE] Skippable pre-recorded Bink video detected (frames=%u, skip=%d)",
                               frames, g_Config.skipPreRecordedVideos ? 1 : 0);
        }
    }

    // Poll XInput gamepad buttons only while a skippable video is active
    bool padSkipPressed = false;
    if (activeBinkRes && g_Config.skipPreRecordedVideos) {
        EnsureXInputLoaded();
        if (g_FnXInputGetState) {
            for (DWORD u = 0; u < 4; ++u) {
                XInputState st{};
                if (g_FnXInputGetState(u, &st) == ERROR_SUCCESS) {
                    const WORD newlyPressed = static_cast<WORD>(st.Gamepad.wButtons & ~g_PrevPadButtons[u]);
                    g_PrevPadButtons[u] = st.Gamepad.wButtons;
                    if (newlyPressed != 0) {
                        padSkipPressed = true;
                    }
                } else {
                    g_PrevPadButtons[u] = 0;
                }
            }
        }
    } else {
        memset(g_PrevPadButtons, 0, sizeof(g_PrevPadButtons));
    }

    const bool keyOrMouseSkip = (InterlockedExchange(&g_PendingSkipRequest, 0) != 0);
    if (activeBinkRes && activeHbink && g_Config.skipPreRecordedVideos &&
        (keyOrMouseSkip || padSkipPressed) &&
        (now - g_ActiveBinkStartTick) >= kVideoSkipGraceMs) {
        // Advance both CBinkResource playedFrames (> startFrame and >= frames) and HBINK->FrameNum
        // so native CBinkResource::IsFinished() (0x151950) cleanly ends playback on the next tick.
        const uint32_t startFrame = *reinterpret_cast<const uint32_t*>(activeBinkRes + 80);
        *reinterpret_cast<uint32_t*>(activeBinkRes + 84) = std::max(frames, startFrame + 1U);
        *reinterpret_cast<uint32_t*>(activeHbink + 12) = frames;
        HookEngine::LogMsg("[LIVE] Pre-recorded Bink video skipped by user input (frame %u/%u)",
                           frameNum, frames);
        g_ActiveSkippableBinkRes = nullptr;
        UpdateDiscretePrompt(nullptr);
        return;
    }

    // Discrete native on-screen feedback (F8 toggle toast OR subtle skip hint during videos)
    if (g_ToggleToastUntilTick != 0 && static_cast<int32_t>(g_ToggleToastUntilTick - now) > 0) {
        UpdateDiscretePrompt(g_ToggleToastText);
    } else {
        g_ToggleToastUntilTick = 0;
        if (activeBinkRes && g_Config.showSkipPrompt) {
            UpdateDiscretePrompt(
                g_Config.skipPreRecordedVideos
                    ? L"[Any Key] Skip Video   |   [F8] Disable"
                    : L"[F8] Enable Video Skip");
        } else {
            UpdateDiscretePrompt(nullptr);
        }
    }
}

bool ContainsCaseInsensitiveA(const char* haystack, const char* needle) {
    if (!haystack || !needle || !*needle) {
        return false;
    }
    const size_t nlen = strlen(needle);
    for (size_t i = 0; i < 128 && haystack[i] != '\0'; ++i) {
        size_t j = 0;
        while (j < nlen && haystack[i + j] != '\0' &&
               std::tolower(static_cast<unsigned char>(haystack[i + j])) ==
                   std::tolower(static_cast<unsigned char>(needle[j]))) {
            ++j;
        }
        if (j == nlen) {
            return true;
        }
    }
    return false;
}

const char* ReadMovieNameSafe(uint8_t* mov) {
    if (!IsValidUserPtr(mov)) {
        return "";
    }
    uint8_t* wrapper = *reinterpret_cast<uint8_t**>(mov + 8);
    if (!IsValidUserPtr(wrapper)) {
        return "";
    }
    void** vt = *reinterpret_cast<void***>(wrapper);
    if (!IsValidUserPtr(vt) || !IsValidUserPtr(vt[1])) {
        return "";
    }
    using FnGetName = const char*(__fastcall*)(void*);
    const char* name = reinterpret_cast<FnGetName>(vt[1])(wrapper);
    return IsValidUserPtr(name) ? name : "";
}

void HideSpriteNodeClean(uint8_t* node) {
    if (!IsValidUserPtr(node) || !g_Sym.fnSpriteNodeSetVis || !g_Sym.fnSpriteNodeInval) {
        return;
    }
    const bool visible = ((*(node + 292) >> 3) & 1) != 0;
    if (!visible) {
        return;
    }
    // Verify internal movie/stage pointers accessed by SpriteNode::SetVisible & Invalidate
    uint8_t* m192 = *reinterpret_cast<uint8_t**>(node + 192);
    uint8_t* m152 = *reinterpret_cast<uint8_t**>(node + 152);
    if (!IsValidUserPtr(m192) || !IsValidUserPtr(m152)) {
        return;
    }
    uint8_t* m136 = *reinterpret_cast<uint8_t**>(m192 + 136);
    if (!IsValidUserPtr(m136) || !IsValidUserPtr(*reinterpret_cast<uint8_t**>(m136 + 1584))) {
        return;
    }
    g_Sym.fnSpriteNodeSetVis(node, 0);
    g_Sym.fnSpriteNodeInval(node);
    *reinterpret_cast<uint32_t*>(node + 100) |= 1U;
}

// Recursively locate and hide Ubisoft's baked 16:9 letterbox bars placed outside 1280x720
// (e.g. PauseRoot tabBar & WorldMap mcVignetteContainer 1280x200 bars at y=-199 / y=+719,
// and WeaponWheel 6000x500 bars at y=+720). Returns count of baked bar nodes hidden.
uint32_t SweepBakedBlackBarsRecursive(
    uint8_t* parentNode,
    uint32_t depth,
    uint32_t& budget,
    bool hideDirectParentOfBars) {
    if (!IsValidUserPtr(parentNode) || depth > 3 || budget == 0) {
        return 0;
    }
    uint8_t* child = *reinterpret_cast<uint8_t**>(parentNode + 176);  // firstChild
    uint32_t hiddenHere = 0;
    uint32_t matchedDirectBars = 0;

    while (IsValidUserPtr(child) && budget > 0) {
        --budget;
        const double sx = std::fabs(static_cast<double>(ReadI32(child + 56)) / 65536.0);
        const double sy = std::fabs(static_cast<double>(ReadI32(child + 68)) / 65536.0);
        const double ty = static_cast<double>(ReadI32(child + 76)) / 20.0;

        // Exact signature of baked letterbox bar sprites:
        // scaleX >= 20x (>=640px wide), scaleY >= 5x (>=160px tall), placed outside y=0..720 stage.
        const bool isBakedBar = (sx >= 20.0 && sy >= 5.0 && (ty <= -150.0 || ty >= 710.0));
        if (isBakedBar) {
            ++matchedDirectBars;
            if (((*(child + 292) >> 3) & 1) != 0) {
                HideSpriteNodeClean(child);
                ++hiddenHere;
            }
        } else if (depth < 3) {
            hiddenHere += SweepBakedBlackBarsRecursive(child, depth + 1, budget, hideDirectParentOfBars);
        }
        child = *reinterpret_cast<uint8_t**>(child + 168);  // nextSibling
    }

    // On WorldMap's lyt_mcFrontUI, hide mcVignetteContainer (depth >= 1) once its baked bars are matched
    // so the 16:9 rectangular vignette border does not frame non-16:9 screens.
    if (hideDirectParentOfBars && depth >= 1 && matchedDirectBars >= 2) {
        if (((*(parentNode + 292) >> 3) & 1) != 0) {
            HideSpriteNodeClean(parentNode);
            ++hiddenHere;
        }
    }
    return hiddenHere;
}

void SweepBakedBlackBarsOnGfxObj(void* gfxObj, bool hideDirectParentOfBars) {
    if (!g_Config.disableBlackBars || !IsValidUserPtr(gfxObj) ||
        !g_Sym.fnSpriteNodeSetVis || !g_Sym.fnSpriteNodeInval) {
        return;
    }
    uint8_t* handle = *reinterpret_cast<uint8_t**>(gfxObj);
    if (!IsValidUserPtr(handle)) {
        return;
    }
    uint8_t* asObj = *reinterpret_cast<uint8_t**>(handle + 48);
    if (!IsValidUserPtr(asObj) || !IsValidUserPtr(*reinterpret_cast<uint8_t**>(asObj + 136))) {
        return;
    }
    uint8_t* spriteNode = *reinterpret_cast<uint8_t**>(asObj + 176);
    if (!IsValidUserPtr(spriteNode)) {
        return;
    }
    uint32_t budget = 192;
    const uint32_t hidden = SweepBakedBlackBarsRecursive(spriteNode, 0, budget, hideDirectParentOfBars);
    if (hidden > 0) {
        static volatile LONG s_loggedBaked = 0;
        if (InterlockedIncrement(&s_loggedBaked) <= 6) {
            HookEngine::LogMsg("[LIVE] Baked 16:9 Flash black-bar child nodes suppressed (count=%u, hideContainer=%d)",
                               hidden, hideDirectParentOfBars ? 1 : 0);
        }
    }
}

void ApplyAdaptiveUiAnchors(double vpW, double vpH) {
    if ((!g_Config.adaptiveUi && !g_Config.disableBlackBars && !g_Config.disableCinematicBlackBars) ||
        !g_Sym.uiMovieMgrPtr || !g_Sym.fnTreeNext || !g_Sym.fnIsValid ||
        !g_Sym.fnSetXScale || !g_Sym.fnSetYScale || !g_Sym.fnSetX || !g_Sym.fnSetY) {
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

        const char* movieName = ReadMovieNameSafe(mov);
        const bool isPressStartMovie = ContainsCaseInsensitiveA(movieName, "PressStart");

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
                        wchar_t aName[96]{};
                        ReadAnchorNameSafe(node, aName, 96);

                        // Capture native SubTitleText2 for discrete video-skip hint / F8 toggle toast
                        if (EqualsCaseInsensitiveW(aName, L"SubTitleText2")) {
                            g_PromptGfxObj = gfxObj;
                        }

                        // Sweep baked 1280x200 / 6000x500 letterbox child sprites inside tabBar,
                        // WorldMap mcFrontUI->mcVignetteContainer, WeaponWheel, etc.
                        const bool isWorldMapFront = EqualsCaseInsensitiveW(aName, L"lyt_mcFrontUI");
                        if (g_Config.disableBlackBars) {
                            SweepBakedBlackBarsOnGfxObj(gfxObj, isWorldMapFront);
                        }

                        const double elemX = *reinterpret_cast<const double*>(obj + 0x10);
                        const double elemY = *reinterpret_cast<const double*>(obj + 0x18);
                        const double curCachedW = *reinterpret_cast<const double*>(obj + 0x20);
                        const double curCachedH = *reinterpret_cast<const double*>(obj + 0x28);
                        const int32_t mode = *reinterpret_cast<const int32_t*>(obj + 0x30);
                        const int32_t scaleFlags = *reinterpret_cast<const int32_t*>(obj + 0x34);
                        const int32_t align = *reinterpret_cast<const int32_t*>(obj + 0x38);
                        const int32_t bScaleX = *reinterpret_cast<const int32_t*>(obj + 0x3c);
                        const int32_t bScaleY = *reinterpret_cast<const int32_t*>(obj + 0x40);

                        const bool isOriginTopLeft = (std::fabs(elemX) < 1.0 && std::fabs(elemY) < 1.0);
                        const bool isOriginCenter = (std::fabs(elemX - 640.0) < 2.0 && std::fabs(elemY - 360.0) < 2.0);
                        const bool isLytAnchor = StartsWithCaseInsensitiveW(aName, L"lyt_");

                        // Classify special UI clips:
                        // 1 = hide black-bar clip (staticBlackBars / BlackBarsMC)
                        // 2 = full-bleed background Cover-Fit (Main Menu Elephant, Pause/Shop nebula,
                        //     Loading Screen, World Map default BG, Press Any Key)
                        // 3 = centered 1280x720 menu stage container (Pause, Shop, Skills, MainMenu, etc.)
                        uint8_t specialKind = 0;
                        if ((g_Config.disableBlackBars && EqualsCaseInsensitiveW(aName, L"staticBlackBars")) ||
                            (g_Config.disableCinematicBlackBars && EqualsCaseInsensitiveW(aName, L"BlackBarsMC")) ||
                            (g_Config.disableBlackBars && ContainsCaseInsensitiveW(aName, L"blackbar"))) {
                            specialKind = 1;
                        } else if (g_Config.adaptiveUi && isLytAnchor && (isOriginTopLeft || isOriginCenter) &&
                                   (EqualsCaseInsensitiveW(aName, L"lyt_mcMainMenuBG") ||
                                    EqualsCaseInsensitiveW(aName, L"lyt_mcBack") ||
                                    EqualsCaseInsensitiveW(aName, L"lyt_mcBG") ||
                                    EqualsCaseInsensitiveW(aName, L"lyt_mcDefaultBG") ||
                                    EqualsCaseInsensitiveW(aName, L"lyt_mcBackground") ||
                                    EqualsCaseInsensitiveW(aName, L"lyt_mcBackgroundOpaque") ||
                                    EqualsCaseInsensitiveW(aName, L"lyt_mcBlackBG") ||
                                    EqualsCaseInsensitiveW(aName, L"lyt_mcPowderBack") ||
                                    EqualsCaseInsensitiveW(aName, L"lyt_art") ||
                                    StartsWithCaseInsensitiveW(aName, L"lyt_bg") ||
                                    ContainsCaseInsensitiveW(aName, L"background") ||
                                    (isPressStartMovie && EqualsCaseInsensitiveW(aName, L"lyt_page")))) {
                            specialKind = 2;
                        } else if (g_Config.adaptiveUi &&
                                   isLytAnchor && anchorCount <= 12 &&
                                   (mode == 2 || mode == 3) && align == 0 && isOriginTopLeft &&
                                   !EqualsCaseInsensitiveW(aName, L"lyt_tabBar") &&
                                   !isWorldMapFront &&
                                   !ContainsCaseInsensitiveW(aName, L"HUD")) {
                            specialKind = 3;
                        }

                        if (!g_Config.adaptiveUi && specialKind != 1) {
                            node = g_Sym.fnTreeNext(node);
                            continue;
                        }

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
                                c.specialKind != specialKind ||
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

                        // Always re-hide black-bar clips in case ActionScript toggled visibility back on
                        if (specialKind == 1 && g_Sym.fnSetVisible) {
                            uint32_t visStatus = 0;
                            g_Sym.fnSetVisible(gfxObj, &visStatus, 0);
                        }

                        if (needsApply) {
                            *reinterpret_cast<double*>(obj + 0x20) = markedW;
                            *reinterpret_cast<double*>(obj + 0x28) = markedH;

                            uint32_t status = 0;
                            if (specialKind == 1) {
                                if (g_Sym.fnSetVisible) {
                                    g_Sym.fnSetVisible(gfxObj, &status, 0);
                                }
                                g_Sym.fnSetXScale(gfxObj, &status, 0.0);
                                g_Sym.fnSetYScale(gfxObj, &status, 0.0);
                                g_Sym.fnSetX(gfxObj, &status, -10000.0);
                                g_Sym.fnSetY(gfxObj, &status, -10000.0);

                                static volatile LONG s_loggedBB = 0;
                                if (InterlockedCompareExchange(&s_loggedBB, 1, 0) == 0) {
                                    HookEngine::LogMsg("[LIVE] Flash BlackBars clip suppressed directly");
                                }
                            } else if (specialKind == 2) {
                                // Full-bleed aspect-preserving Cover-Fit (origin-invariant for both (0,0) and (640,360))
                                const double coverScale = std::max(vpW / 1280.0, vpH / 720.0);
                                const double outX = 0.5 * vpW - coverScale * (640.0 - elemX);
                                const double outY = 0.5 * vpH - coverScale * (360.0 - elemY);
                                g_Sym.fnSetXScale(gfxObj, &status, coverScale);
                                g_Sym.fnSetYScale(gfxObj, &status, coverScale);
                                g_Sym.fnSetX(gfxObj, &status, outX);
                                g_Sym.fnSetY(gfxObj, &status, outY);

                                static volatile LONG s_loggedBg = 0;
                                if (InterlockedIncrement(&s_loggedBg) <= 6) {
                                    char aBuf[64]{};
                                    size_t k = 0;
                                    for (; k + 1 < sizeof(aBuf) && aName[k] != 0; ++k) {
                                        aBuf[k] = (aName[k] >= 0x20 && aName[k] <= 0x7E) ? static_cast<char>(aName[k]) : '?';
                                    }
                                    HookEngine::LogMsg(
                                        "[LIVE] Menu background Cover-Fit applied '%s' (vp=%.0fx%.0f scale=%.4f pos=(%.1f,%.1f))",
                                        aBuf, vpW, vpH, coverScale, outX, outY);
                                }
                            } else if (specialKind == 3) {
                                // Center full-stage 1280x720 menu container cleanly over Cover-Fit backdrop
                                const double uScale = std::min(vpW / 1280.0, vpH / 720.0);
                                const double outX = 0.5 * (vpW - 1280.0 * uScale);
                                const double outY = 0.5 * (vpH - 720.0 * uScale);
                                g_Sym.fnSetXScale(gfxObj, &status, uScale);
                                g_Sym.fnSetYScale(gfxObj, &status, uScale);
                                g_Sym.fnSetX(gfxObj, &status, outX);
                                g_Sym.fnSetY(gfxObj, &status, outY);

                                static volatile LONG s_loggedPage = 0;
                                if (InterlockedIncrement(&s_loggedPage) <= 6) {
                                    char aBuf[64]{};
                                    size_t k = 0;
                                    for (; k + 1 < sizeof(aBuf) && aName[k] != 0; ++k) {
                                        aBuf[k] = (aName[k] >= 0x20 && aName[k] <= 0x7E) ? static_cast<char>(aName[k]) : '?';
                                    }
                                    HookEngine::LogMsg(
                                        "[LIVE] Menu stage centered '%s' (vp=%.0fx%.0f scale=%.4f pos=(%.1f,%.1f))",
                                        aBuf, vpW, vpH, uScale, outX, outY);
                                }
                            } else {
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

                                static volatile LONG s_loggedModeMask = 0;
                                const LONG bit = (mode >= 0 && mode <= 5) ? (1L << mode) : (1L << 6);
                                if ((InterlockedOr(&s_loggedModeMask, bit) & bit) == 0) {
                                    HookEngine::LogMsg(
                                        "[LIVE] AdaptiveUI applied (vp=%.0fx%.0f mode=%d scale=%.4fx%.4f bounds=[%.0f,%.0f,%.0f,%.0f])",
                                        vpW, vpH, mode, io.scaleX, io.scaleY,
                                        io.safeL, io.safeT, io.safeR, io.safeB);
                                }
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
                            entry.specialKind = specialKind;
                            g_AnchorCache[obj] = entry;
                        }
                    }
                }
            }
            node = g_Sym.fnTreeNext(node);
        }
    }
}

bool IsIgnoredSkipKey(WPARAM vk) {
    switch (vk) {
        case VK_F8:
        case VK_SHIFT:
        case VK_LSHIFT:
        case VK_RSHIFT:
        case VK_CONTROL:
        case VK_LCONTROL:
        case VK_RCONTROL:
        case VK_MENU:
        case VK_LMENU:
        case VK_RMENU:
        case VK_LWIN:
        case VK_RWIN:
        case VK_CAPITAL:
        case VK_NUMLOCK:
        case VK_SCROLL:
        case VK_SNAPSHOT:
        case VK_F12:
            return true;
        default:
            return false;
    }
}

LRESULT CALLBACK Hook_WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == kMsgStartTimer) {
        SetTimer(hWnd, kAdaptiveTimerId, kAdaptiveTimerMs, nullptr);
        TickAdaptiveEngineOnMainThread();
        return 0;
    }
    if (msg == WM_TIMER && wParam == kAdaptiveTimerId) {
        static volatile LONG s_loggedTimer = 0;
        if (InterlockedCompareExchange(&s_loggedTimer, 1, 0) == 0) {
            HookEngine::LogMsg("[LIVE] Main-thread WM_TIMER heartbeat active (HWND=0x%p)", hWnd);
        }
        TickAdaptiveEngineOnMainThread();
        return 0;
    }
    if (msg == WM_SIZE || msg == WM_DISPLAYCHANGE || msg == WM_ACTIVATEAPP) {
        TickAdaptiveEngineOnMainThread();
    }

    // Discrete in-game F8 toggle & any-button pre-recorded video skip
    if (msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN) {
        const bool isRepeat = (lParam & (1LL << 30)) != 0;
        if (!isRepeat) {
            if (wParam == VK_F8) {
                TogglePreRecordedVideoSkipInGame();
            } else if (g_ActiveSkippableBinkRes != nullptr &&
                       g_Config.skipPreRecordedVideos &&
                       !IsIgnoredSkipKey(wParam)) {
                InterlockedExchange(&g_PendingSkipRequest, 1);
            }
        }
    } else if (msg == WM_LBUTTONDOWN || msg == WM_RBUTTONDOWN || msg == WM_MBUTTONDOWN) {
        if (g_ActiveSkippableBinkRes != nullptr && g_Config.skipPreRecordedVideos) {
            InterlockedExchange(&g_PendingSkipRequest, 1);
        }
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
    const bool titleMatch = (wcsstr(title, L"Far Cry") != nullptr || wcsstr(title, L"FarCry") != nullptr);
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
            wchar_t title[128]{};
            DWORD_PTR textRes = 0;
            SendMessageTimeoutW(
                ctx.bestHwnd, WM_GETTEXT, 128, reinterpret_cast<LPARAM>(title),
                SMTO_ABORTIFHUNG | SMTO_NORMAL, 50, &textRes);
            char titleAscii[128]{};
            size_t ti = 0;
            for (size_t i = 0; title[i] != 0 && ti + 1 < sizeof(titleAscii); ++i) {
                titleAscii[ti++] = (title[i] >= 0x20 && title[i] <= 0x7E) ? static_cast<char>(title[i]) : ' ';
            }
            titleAscii[ti] = '\0';

            WNDPROC prevOrig = reinterpret_cast<WNDPROC>(
                InterlockedCompareExchangePointer(reinterpret_cast<PVOID volatile*>(&g_OrigWndProc), nullptr, nullptr));
            if (curWnd && IsWindow(curWnd) && prevOrig && prevOrig != &Hook_WndProc) {
                KillTimer(curWnd, kAdaptiveTimerId);
                SetWindowLongPtrW(curWnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(prevOrig));
            }

            WNDPROC targetOrig = reinterpret_cast<WNDPROC>(GetWindowLongPtrW(ctx.bestHwnd, GWLP_WNDPROC));
            if (targetOrig && targetOrig != &Hook_WndProc) {
                InterlockedExchangePointer(reinterpret_cast<PVOID volatile*>(&g_OrigWndProc),
                                           reinterpret_cast<PVOID>(targetOrig));
                InterlockedExchangePointer(reinterpret_cast<PVOID volatile*>(&g_SubclassedWnd),
                                           reinterpret_cast<PVOID>(ctx.bestHwnd));
                SetWindowLongPtrW(ctx.bestHwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&Hook_WndProc));
                PostMessageW(ctx.bestHwnd, kMsgStartTimer, 0, 0);
                HookEngine::LogMsg("[LIVE] Attached main-thread adaptive timer to HWND=0x%p ('%s')",
                                   ctx.bestHwnd, titleAscii);
            }
        }

        Sleep(250);
    }
    return 0;
}

void DumpDiagnosticsIfEnabled() {
    if (!g_Config.diagnosticLog || !g_Sym.hFC64) {
        return;
    }
    static uint32_t s_tick = 0;
    ++s_tick;
    if (s_tick != 1 && (s_tick % 90) != 0) {
        return;
    }
    if (s_tick > 1800) {
        return;
    }

    if (g_Sym.uiMovieMgrPtr && IsValidUserPtr(*g_Sym.uiMovieMgrPtr)) {
        uint8_t* mgr = *g_Sym.uiMovieMgrPtr;
        const uint32_t movieCount = *reinterpret_cast<const uint32_t*>(mgr + 16);
        void** movieItems = *reinterpret_cast<void***>(mgr + 8);
        HookEngine::LogMsg("[DIAG] tick=%u movies=%u", s_tick, movieCount);
        if (IsValidUserPtr(movieItems) && movieCount <= 128) {
            for (uint32_t m = 0; m < movieCount; ++m) {
                auto* mov = reinterpret_cast<uint8_t*>(movieItems[m]);
                if (!IsValidUserPtr(mov)) continue;
                const char* mName = "?";
                uint8_t* wrapper = *reinterpret_cast<uint8_t**>(mov + 8);
                uint16_t st172 = 0, st174 = 0, st176 = 0, st178 = 0;
                if (IsValidUserPtr(wrapper)) {
                    void** vt = *reinterpret_cast<void***>(wrapper);
                    if (IsValidUserPtr(vt) && IsValidUserPtr(vt[1])) {
                        using FnGetName = const char*(__fastcall*)(void*);
                        const char* n = reinterpret_cast<FnGetName>(vt[1])(wrapper);
                        if (IsValidUserPtr(n)) mName = n;
                    }
                    uint8_t* inner = *reinterpret_cast<uint8_t**>(wrapper + 8);
                    if (IsValidUserPtr(inner)) {
                        uint8_t* gfxMov = *reinterpret_cast<uint8_t**>(inner + 160);
                        if (IsValidUserPtr(gfxMov)) {
                            st172 = *reinterpret_cast<const uint16_t*>(gfxMov + 172);
                            st174 = *reinterpret_cast<const uint16_t*>(gfxMov + 174);
                            st176 = *reinterpret_cast<const uint16_t*>(gfxMov + 176);
                            st178 = *reinterpret_cast<const uint16_t*>(gfxMov + 178);
                        }
                    }
                }
                uint8_t* anchorMgr = *reinterpret_cast<uint8_t**>(mov + 24);
                uint32_t anchorCount = 0;
                if (IsValidUserPtr(anchorMgr)) {
                    anchorCount = *reinterpret_cast<const uint32_t*>(anchorMgr + 24 + 32);
                }
                HookEngine::LogMsg("[DIAG]  Movie[%u] '%s' stage=%ux%u vp=%ux%u anchors=%u",
                                   m, mName, st174, st172, st178, st176, anchorCount);
                if (IsValidUserPtr(anchorMgr) && anchorCount > 0 && anchorCount <= 128 && g_Sym.fnTreeNext) {
                    uint8_t* treeHead = anchorMgr + 24;
                    void* node = *reinterpret_cast<void**>(treeHead);
                    uint32_t vis = 0;
                    while (IsValidUserPtr(node) && node != treeHead && vis < anchorCount + 4) {
                        ++vis;
                        wchar_t wName[96]{};
                        ReadAnchorNameSafe(node, wName, 96);
                        char aNameBuf[96]{};
                        size_t k = 0;
                        for (; k + 1 < sizeof(aNameBuf) && wName[k] != 0; ++k) {
                            aNameBuf[k] = (wName[k] >= 0x20 && wName[k] <= 0x7E) ? static_cast<char>(wName[k]) : '?';
                        }
                        aNameBuf[k] = '\0';
                        auto* obj = *reinterpret_cast<uint8_t**>(reinterpret_cast<uint8_t*>(node) + 40);
                        if (IsValidUserPtr(obj)) {
                            HookEngine::LogMsg(
                                "[DIAG]    Anchor '%s' mode=%d align=%d sf=%d pos=(%.1f,%.1f)",
                                aNameBuf,
                                *reinterpret_cast<const int32_t*>(obj + 0x30),
                                *reinterpret_cast<const int32_t*>(obj + 0x38),
                                *reinterpret_cast<const int32_t*>(obj + 0x34),
                                *reinterpret_cast<const double*>(obj + 0x10),
                                *reinterpret_cast<const double*>(obj + 0x18));
                        }
                        node = g_Sym.fnTreeNext(node);
                    }
                }
            }
        }
    }
}

}  // namespace

void LoadPatchConfig(HMODULE hSelf, PatchConfig& outConfig) {
    outConfig = PatchConfig{};
    wchar_t dir[MAX_PATH]{};
    HookEngine::ResolveModuleDir(hSelf, dir, MAX_PATH);
    _snwprintf_s(g_IniPath, MAX_PATH, _TRUNCATE, L"%s\\FC4_AspectFix.ini", dir);

    outConfig.enabled = GetPrivateProfileIntW(L"General", L"Enabled", 1, g_IniPath) != 0;
    outConfig.diagnosticLog = GetPrivateProfileIntW(L"General", L"DiagnosticLog", 0, g_IniPath) != 0;
    outConfig.disableBlackBars = GetPrivateProfileIntW(L"BlackBars", L"DisableBlackBars", 1, g_IniPath) != 0;
    outConfig.disableCinematicBlackBars =
        GetPrivateProfileIntW(L"BlackBars", L"DisableCinematicBlackBars", 1, g_IniPath) != 0;
    outConfig.adaptiveUi = GetPrivateProfileIntW(L"UI", L"AdaptiveUI", 1, g_IniPath) != 0;
    outConfig.fixFov = GetPrivateProfileIntW(L"FOV", L"FixFOV", 1, g_IniPath) != 0;
    outConfig.unlockResolutions =
        GetPrivateProfileIntW(L"Display", L"UnlockResolutions", 1, g_IniPath) != 0;
    outConfig.fixVideoAspectRatio =
        GetPrivateProfileIntW(L"Video", L"FixVideoAspectRatio", 1, g_IniPath) != 0;
    outConfig.skipPreRecordedVideos =
        GetPrivateProfileIntW(L"Video", L"SkipPreRecordedVideos", 1, g_IniPath) != 0;
    outConfig.showSkipPrompt =
        GetPrivateProfileIntW(L"Video", L"ShowSkipPrompt", 1, g_IniPath) != 0;
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
        return g_Sym.ready ? 17 : 0;
    }

    LoadPatchConfig(hSelf, g_Config);
    HookEngine::LogMsg(
        "[CONFIG] Enabled=%d DisableBlackBars=%d DisableCinematicBlackBars=%d "
        "AdaptiveUI=%d FixFOV=%d UnlockResolutions=%d FixVideoAR=%d SkipVideos=%d ShowSkipPrompt=%d",
        g_Config.enabled ? 1 : 0,
        g_Config.disableBlackBars ? 1 : 0,
        g_Config.disableCinematicBlackBars ? 1 : 0,
        g_Config.adaptiveUi ? 1 : 0,
        g_Config.fixFov ? 1 : 0,
        g_Config.unlockResolutions ? 1 : 0,
        g_Config.fixVideoAspectRatio ? 1 : 0,
        g_Config.skipPreRecordedVideos ? 1 : 0,
        g_Config.showSkipPrompt ? 1 : 0);

    if (!g_Config.enabled) {
        HookEngine::LogMsg("[INFO] Disabled via FC4_AspectFix.ini");
        return 0;
    }

    HookEngine::LogMsg("[INFO] FC64.dll base=0x%p (read-only AOB discovery; zero .text writes)", hFC64);

    g_Sym = EngineSymbols{};
    g_Sym.hFC64 = hFC64;
    int resolved = 0;
    uint32_t matches = 0;

    // 1. g_DisplayManagerPtr (RVA 0x2dd84c8)
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

    // 10. BlackBars service lookup + vtable targets + Sym11 Scaleform SetVisible (0x1cb9550)
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

        if (pSetVis[0x3D] == 0xE8) {
            g_Sym.fnSetVisible = reinterpret_cast<FnSetVisible>(
                const_cast<uint8_t*>(HookEngine::ResolveRipRelative(pSetVis + 0x42, ReadI32(pSetVis + 0x3E))));
            HookEngine::LogMsg("[OK] Sym11_ScaleformSetVisible -> RVA 0x%llX",
                               RvaOf(hFC64, reinterpret_cast<const void*>(g_Sym.fnSetVisible)));
            ++resolved;
        }
    }

    // 12. Scaleform SetText (RVA 0x1cb99f0)
    if (const uint8_t* p = HookEngine::FindPatternInModule(
            hFC64,
            "Sym12_ScaleformSetText",
            "c0 74 04 4c 8d 40 18 48 8b 49 10 48 8d 94 24 58 01 00 00 e8 ?? ?? ?? ?? "
            "8b 08 89 8c 24 40 01 00 00 48 8d 8c 24 40 01 00 00 e8 ?? ?? ?? ?? 8b 47",
            0x74a455,
            matches);
        p && matches == 1) {
        g_Sym.fnSetText = reinterpret_cast<FnSetText>(
            const_cast<uint8_t*>(HookEngine::ResolveRipRelative(p + 24, ReadI32(p + 20))));
        HookEngine::LogMsg("[OK] Sym12_ScaleformSetText -> RVA 0x%llX",
                           RvaOf(hFC64, reinterpret_cast<const void*>(g_Sym.fnSetText)));
        ++resolved;
    }

    // 13. g_Mesh2DPoolHeaderPtr (RVA 0x2d1e438 = &g_Mesh2DPool + 24) from BinkQuadSetup (0x1532d1a)
    if (const uint8_t* p = HookEngine::FindPatternInModule(
            hFC64,
            "Sym13_Mesh2DPool",
            "48 8d 0d ?? ?? ?? ?? 41 b0 01 48 8b d3 f3 0f 11 45 af 0f 28 45 a7 0f 29 45 c7 "
            "e8 ?? ?? ?? ?? 48 8b c8 4c 8b e0 e8 ?? ?? ?? ?? b2 01 49 8b cc",
            0x1532d1a,
            matches);
        p && matches == 1) {
        const uint8_t* poolObj = HookEngine::ResolveRipRelative(p + 7, ReadI32(p + 3));
        g_Sym.mesh2DPoolHeaderPtr = reinterpret_cast<uint8_t**>(const_cast<uint8_t*>(poolObj + 24));
        HookEngine::LogMsg("[OK] Sym13_Mesh2DPool -> HeaderPtr RVA 0x%llX",
                           RvaOf(hFC64, g_Sym.mesh2DPoolHeaderPtr));
        ++resolved;
    }

    // 14. g_BinkPlayerMgrPtr (RVA 0x2e4c4a8) + optional g_Video2DArrayHeaderPtr (RVA 0x2d1dc98)
    if (const uint8_t* p = HookEngine::FindPatternInModule(
            hFC64,
            "Sym14_BinkPlayerMgr",
            "48 89 4c 24 08 53 48 83 ec 20 e8 ?? ?? ?? ?? 48 8b d8 ff 40 08 48 83 78 10 00 74 23 "
            "48 8b 4c 24 30 e8 ?? ?? ?? ?? 48 8d 54 24 38 48 89 44 24 38 ff 40 08 48 8b 0d ?? ?? ?? ??",
            0x190a940,
            matches);
        p && matches == 1) {
        g_Sym.binkPlayerMgrPtr = reinterpret_cast<uint8_t**>(
            const_cast<uint8_t*>(HookEngine::ResolveRipRelative(p + 58, ReadI32(p + 54))));
        HookEngine::LogMsg("[OK] Sym14_BinkPlayerMgr -> RVA 0x%llX",
                           RvaOf(hFC64, g_Sym.binkPlayerMgrPtr));
        ++resolved;
    }

    if (const uint8_t* p = HookEngine::FindPatternInModule(
            hFC64,
            "Sym14B_Video2DArray",
            "48 8b 05 ?? ?? ?? ?? 41 81 e3 ff ff ff 00 4d 8b f9 4c 8b 10 8b 43 28 49 8b f8 "
            "4f 8b 34 da 48 8b f2 4c 8b e1 3b 43 20 75",
            0x2b5391,
            matches);
        p && matches == 1) {
        g_Sym.video2DArrayHeaderPtr = reinterpret_cast<uint8_t**>(
            const_cast<uint8_t*>(HookEngine::ResolveRipRelative(p + 7, ReadI32(p + 3))));
    }

    // 15. FireUI SpriteNode::SetVisible (RVA 0x1d32b00)
    if (const uint8_t* p = HookEngine::FindPatternInModule(
            hFC64,
            "Sym15_SpriteNodeSetVisible",
            "48 83 ec 28 0f b6 81 24 01 00 00 4c 8b c9 c0 e8 03 24 01 3a d0 74 20 48",
            0x1d32b00,
            matches);
        p && matches == 1) {
        g_Sym.fnSpriteNodeSetVis = reinterpret_cast<FnSpriteNodeSetVis>(const_cast<uint8_t*>(p));
        HookEngine::LogMsg("[OK] Sym15_SpriteNodeSetVisible -> RVA 0x%llX",
                           RvaOf(hFC64, reinterpret_cast<const void*>(g_Sym.fnSpriteNodeSetVis)));
        ++resolved;
    }

    // 16. FireUI SpriteNode::Invalidate (RVA 0x1d2c180)
    if (const uint8_t* p = HookEngine::FindPatternInModule(
            hFC64,
            "Sym16_SpriteNodeInvalidate",
            "48 89 5c 24 08 57 48 83 ec 20 48 8b d9 48 8b 89 00 01 00 00 40 32 ff 48",
            0x1d2c180,
            matches);
        p && matches == 1) {
        g_Sym.fnSpriteNodeInval = reinterpret_cast<FnSpriteNodeInval>(const_cast<uint8_t*>(p));
        HookEngine::LogMsg("[OK] Sym16_SpriteNodeInvalidate -> RVA 0x%llX",
                           RvaOf(hFC64, reinterpret_cast<const void*>(g_Sym.fnSpriteNodeInval)));
        ++resolved;
    }

    // 17. CBinkResource VTable (RVA 0x26d50f0) from CBinkResource::ctor (0x197b60)
    if (const uint8_t* p = HookEngine::FindPatternInModule(
            hFC64,
            "Sym17_BinkResourceVTable",
            "40 53 48 83 ec 20 48 8b d9 e8 ?? ?? ?? ?? 48 8d 05 ?? ?? ?? ?? 48 89 43 10 "
            "33 c0 4c 8d 1d ?? ?? ?? ?? 4c 89 1b 48 89 83 08 01 00 00 48 89 83 10 01 00 00 "
            "48 89 83 18 01 00 00 48 89 83 58 01 00 00",
            0x197b60,
            matches);
        p && matches == 1) {
        g_Sym.binkResourceVTable = HookEngine::ResolveRipRelative(p + 34, ReadI32(p + 30));
        HookEngine::LogMsg("[OK] Sym17_BinkResourceVTable -> RVA 0x%llX",
                           RvaOf(hFC64, g_Sym.binkResourceVTable));
        ++resolved;
    }

    g_Sym.ready = (resolved == 17);
    HookEngine::LogMsg("[SUMMARY] Resolved %d/17 engine symbols (zero .text bytes modified)", resolved);
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
    FixBinkVideoQuads();
    UpdatePreRecordedVideoSkip();
    TryInstallModeProviderHook();
    TryInstallBlackBarsHook();
    DumpDiagnosticsIfEnabled();
}

void StartAdaptiveController(HMODULE hSelf, HMODULE hFC64) {
    if (InitializeEngineSymbols(hSelf, hFC64) != 17) {
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
