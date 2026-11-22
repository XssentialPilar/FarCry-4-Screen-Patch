#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdint>
#include <cstdio>

#include "hook_engine.h"
#include "patch_main.h"

namespace {

using FnDirectInput8Create = HRESULT(WINAPI*)(HINSTANCE, DWORD, const GUID*, void**, void*);
using FnDllCanUnloadNow = HRESULT(WINAPI*)(void);
using FnDllGetClassObject = HRESULT(WINAPI*)(const GUID*, const GUID*, void**);
using FnDllRegisterServer = HRESULT(WINAPI*)(void);
using FnDllUnregisterServer = HRESULT(WINAPI*)(void);
using FnGetdfDIJoystick = const void*(WINAPI*)(void);
using FnRunGame = bool(__fastcall*)(HINSTANCE, const char*);

HMODULE g_hSelf = nullptr;
HMODULE g_hRealDinput8 = nullptr;
INIT_ONCE g_RealDinputOnce = INIT_ONCE_STATIC_INIT;

FnDirectInput8Create g_RealDirectInput8Create = nullptr;
FnDllCanUnloadNow g_RealDllCanUnloadNow = nullptr;
FnDllGetClassObject g_RealDllGetClassObject = nullptr;
FnDllRegisterServer g_RealDllRegisterServer = nullptr;
FnDllUnregisterServer g_RealDllUnregisterServer = nullptr;
FnGetdfDIJoystick g_RealGetdfDIJoystick = nullptr;

FnRunGame g_OrigRunGame = nullptr;

BOOL CALLBACK LoadRealDinput8Callback(PINIT_ONCE, PVOID, PVOID*) {
    wchar_t sysDir[MAX_PATH]{};
    const UINT len = GetSystemDirectoryW(sysDir, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) {
        return TRUE;
    }
    wchar_t fullPath[MAX_PATH]{};
    _snwprintf_s(fullPath, MAX_PATH, _TRUNCATE, L"%s\\dinput8.dll", sysDir);
    g_hRealDinput8 = LoadLibraryW(fullPath);
    if (g_hRealDinput8) {
        g_RealDirectInput8Create = reinterpret_cast<FnDirectInput8Create>(
            GetProcAddress(g_hRealDinput8, "DirectInput8Create"));
        g_RealDllCanUnloadNow = reinterpret_cast<FnDllCanUnloadNow>(
            GetProcAddress(g_hRealDinput8, "DllCanUnloadNow"));
        g_RealDllGetClassObject = reinterpret_cast<FnDllGetClassObject>(
            GetProcAddress(g_hRealDinput8, "DllGetClassObject"));
        g_RealDllRegisterServer = reinterpret_cast<FnDllRegisterServer>(
            GetProcAddress(g_hRealDinput8, "DllRegisterServer"));
        g_RealDllUnregisterServer = reinterpret_cast<FnDllUnregisterServer>(
            GetProcAddress(g_hRealDinput8, "DllUnregisterServer"));
        g_RealGetdfDIJoystick = reinterpret_cast<FnGetdfDIJoystick>(
            GetProcAddress(g_hRealDinput8, "GetdfDIJoystick"));
    }
    return TRUE;
}

void EnsureRealDinput8() {
    InitOnceExecuteOnce(&g_RealDinputOnce, LoadRealDinput8Callback, nullptr, nullptr);
}

LONG CALLBACK FatalExceptionLogger(EXCEPTION_POINTERS* ep) {
    if (!ep || !ep->ExceptionRecord) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    const DWORD code = ep->ExceptionRecord->ExceptionCode;
    if (code == EXCEPTION_ACCESS_VIOLATION ||
        code == EXCEPTION_ILLEGAL_INSTRUCTION ||
        code == EXCEPTION_PRIV_INSTRUCTION ||
        code == EXCEPTION_INT_DIVIDE_BY_ZERO ||
        code == EXCEPTION_STACK_OVERFLOW ||
        code == EXCEPTION_IN_PAGE_ERROR) {
        void* addr = ep->ExceptionRecord->ExceptionAddress;
        HMODULE hMod = nullptr;
        wchar_t modPath[MAX_PATH]{};
        if (GetModuleHandleExW(
                GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCWSTR>(addr),
                &hMod) &&
            hMod) {
            GetModuleFileNameW(hMod, modPath, MAX_PATH);
        }
        const uint64_t rva = hMod
            ? (reinterpret_cast<uintptr_t>(addr) - reinterpret_cast<uintptr_t>(hMod))
            : 0;
        const wchar_t* modName = wcsrchr(modPath, L'\\');
        modName = modName ? (modName + 1) : modPath;
        char modAnsi[128]{};
        WideCharToMultiByte(CP_ACP, 0, modName, -1, modAnsi, sizeof(modAnsi), nullptr, nullptr);
        HookEngine::LogMsg(
            "[CRASH] code=0x%08lX addr=0x%p (%s + 0x%llX)",
            code,
            addr,
            modAnsi[0] ? modAnsi : "unknown",
            static_cast<unsigned long long>(rva));
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

bool __fastcall Hook_RunGame(HINSTANCE hInst, const char* cmdLine) {
    HookEngine::LogInit(g_hSelf, L"FC4_AspectFix.log");
    AddVectoredExceptionHandler(1, FatalExceptionLogger);
    HookEngine::LogMsg("[TRACE] Entering original FC64!RunGame (orig=0x%p, tick=%lu)",
                       reinterpret_cast<void*>(g_OrigRunGame), GetTickCount());
    bool ret = false;
    if (g_OrigRunGame) {
        ret = g_OrigRunGame(hInst, cmdLine);
    }
    HookEngine::LogMsg("[TRACE] Original FC64!RunGame returned %d (tick=%lu)",
                       ret ? 1 : 0, GetTickCount());
    return ret;
}

}  // namespace

extern "C" {

HRESULT WINAPI Proxy_DirectInput8Create(
    HINSTANCE hinst,
    DWORD dwVersion,
    const GUID* riidltf,
    void** ppvOut,
    void* punkOuter) {
    EnsureRealDinput8();
    HRESULT hr = E_FAIL;
    if (g_RealDirectInput8Create) {
        hr = g_RealDirectInput8Create(hinst, dwVersion, riidltf, ppvOut, punkOuter);
    }
    HMODULE hFC64 = GetModuleHandleW(L"FC64.dll");
    if (hFC64) {
        StartAdaptiveController(g_hSelf, hFC64);
    }
    return hr;
}

HRESULT WINAPI Proxy_DllCanUnloadNow(void) {
    EnsureRealDinput8();
    if (!g_RealDllCanUnloadNow) {
        return S_FALSE;
    }
    return g_RealDllCanUnloadNow();
}

HRESULT WINAPI Proxy_DllGetClassObject(const GUID* rclsid, const GUID* riid, void** ppv) {
    EnsureRealDinput8();
    if (!g_RealDllGetClassObject) {
        return CLASS_E_CLASSNOTAVAILABLE;
    }
    return g_RealDllGetClassObject(rclsid, riid, ppv);
}

HRESULT WINAPI Proxy_DllRegisterServer(void) {
    EnsureRealDinput8();
    if (!g_RealDllRegisterServer) {
        return E_FAIL;
    }
    return g_RealDllRegisterServer();
}

HRESULT WINAPI Proxy_DllUnregisterServer(void) {
    EnsureRealDinput8();
    if (!g_RealDllUnregisterServer) {
        return E_FAIL;
    }
    return g_RealDllUnregisterServer();
}

const void* WINAPI Proxy_GetdfDIJoystick(void) {
    EnsureRealDinput8();
    if (!g_RealGetdfDIJoystick) {
        return nullptr;
    }
    return g_RealGetdfDIJoystick();
}

int WINAPI FC4_AspectFix_TestApply(HMODULE hFC64) {
    return InitializeEngineSymbols(g_hSelf, hFC64);
}

}  // extern "C"

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ulReason, LPVOID lpvReserved) {
    if (ulReason == DLL_PROCESS_ATTACH) {
        g_hSelf = hModule;
        DisableThreadLibraryCalls(hModule);

        HMODULE hExe = GetModuleHandleW(nullptr);
        if (hExe) {
            HookEngine::HookImageImportIAT(
                hExe,
                "FC64.dll",
                "?RunGame@@YA_NPEAUHINSTANCE__@@PEBD@Z",
                reinterpret_cast<void*>(&Hook_RunGame),
                reinterpret_cast<void**>(&g_OrigRunGame));
        }
    } else if (ulReason == DLL_PROCESS_DETACH && lpvReserved == nullptr) {
        ShutdownAdaptiveController();
    }
    return TRUE;
}
