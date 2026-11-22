#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cctype>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <share.h>
#include <vector>

namespace HookEngine {

struct LogState {
    CRITICAL_SECTION cs{};
    bool csInitialized = false;
    FILE* file = nullptr;
    wchar_t dirPath[MAX_PATH]{};
};

inline LogState& GetLogState() {
    static LogState state;
    return state;
}

inline void ResolveModuleDir(HMODULE hSelf, wchar_t* outDir, size_t maxChars) {
    wchar_t fullPath[MAX_PATH]{};
    DWORD len = GetModuleFileNameW(hSelf, fullPath, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) {
        wcsncpy_s(outDir, maxChars, L".", _TRUNCATE);
        return;
    }
    wchar_t* lastSlash = wcsrchr(fullPath, L'\\');
    if (lastSlash) {
        *lastSlash = L'\0';
    }
    wcsncpy_s(outDir, maxChars, fullPath, _TRUNCATE);
}

inline void LogInit(HMODULE hSelf, const wchar_t* logFileName) {
    LogState& s = GetLogState();
    if (!s.csInitialized) {
        InitializeCriticalSection(&s.cs);
        s.csInitialized = true;
    }
    EnterCriticalSection(&s.cs);
    ResolveModuleDir(hSelf, s.dirPath, MAX_PATH);
    if (!s.file) {
        wchar_t logPath[MAX_PATH]{};
        _snwprintf_s(logPath, MAX_PATH, _TRUNCATE, L"%s\\%s", s.dirPath, logFileName);
        s.file = _wfsopen(logPath, L"w", _SH_DENYNO);
        if (s.file) {
            fprintf(s.file, "=== FC4_AspectFix V2 (Zero-.text-Edit) Initialized ===\n");
            fflush(s.file);
        }
    }
    LeaveCriticalSection(&s.cs);
}

inline void LogMsg(const char* fmt, ...) {
    LogState& s = GetLogState();
    if (!s.csInitialized || !s.file) {
        return;
    }
    EnterCriticalSection(&s.cs);
    if (s.file) {
        va_list args;
        va_start(args, fmt);
        vfprintf(s.file, fmt, args);
        va_end(args);
        fputc('\n', s.file);
        fflush(s.file);
    }
    LeaveCriticalSection(&s.cs);
}

inline bool WriteMem(void* dst, const void* src, size_t len) {
    if (!dst || !src || len == 0) {
        return false;
    }
    DWORD oldProtect = 0;
    if (!VirtualProtect(dst, len, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        return false;
    }
    memcpy(dst, src, len);
    DWORD unused = 0;
    VirtualProtect(dst, len, oldProtect, &unused);
    FlushInstructionCache(GetCurrentProcess(), dst, len);
    return true;
}

struct PatternByte {
    uint8_t value;
    bool wildcard;
};

inline bool ParsePattern(const char* pattern, std::vector<PatternByte>& out) {
    out.clear();
    if (!pattern) {
        return false;
    }
    const char* p = pattern;
    while (*p) {
        while (*p && std::isspace(static_cast<unsigned char>(*p))) {
            ++p;
        }
        if (!*p) {
            break;
        }
        if (p[0] == '?') {
            out.push_back({0, true});
            ++p;
            if (*p == '?') {
                ++p;
            }
        } else {
            if (!std::isxdigit(static_cast<unsigned char>(p[0])) ||
                !std::isxdigit(static_cast<unsigned char>(p[1]))) {
                return false;
            }
            char hex[3] = {p[0], p[1], '\0'};
            uint8_t val = static_cast<uint8_t>(strtoul(hex, nullptr, 16));
            out.push_back({val, false});
            p += 2;
        }
    }
    return !out.empty();
}

inline bool MatchPatternAt(const uint8_t* mem, const std::vector<PatternByte>& pat) {
    for (size_t i = 0; i < pat.size(); ++i) {
        if (!pat[i].wildcard && mem[i] != pat[i].value) {
            return false;
        }
    }
    return true;
}

inline bool GetTextSectionBounds(HMODULE hMod, const uint8_t*& outStart, size_t& outSize) {
    if (!hMod) {
        return false;
    }
    const auto* base = reinterpret_cast<const uint8_t*>(hMod);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
        return false;
    }
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE ||
        nt->FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64) {
        return false;
    }
    const IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        if (strncmp(reinterpret_cast<const char*>(sec[i].Name), ".text", 5) == 0) {
            outStart = base + sec[i].VirtualAddress;
            outSize = sec[i].Misc.VirtualSize ? sec[i].Misc.VirtualSize : sec[i].SizeOfRawData;
            return true;
        }
    }
    return false;
}

// Read-only AOB scanner in .text (never modifies FC64.dll code)
inline const uint8_t* FindPatternInModule(
    HMODULE hMod,
    const char* name,
    const char* pattern,
    uint64_t expectedRva,
    uint32_t& outMatchCount) {
    outMatchCount = 0;
    std::vector<PatternByte> pat;
    if (!ParsePattern(pattern, pat)) {
        LogMsg("[ERROR] %s: invalid AOB pattern", name);
        return nullptr;
    }

    const uint8_t* textStart = nullptr;
    size_t textSize = 0;
    if (!GetTextSectionBounds(hMod, textStart, textSize) || textSize < pat.size()) {
        LogMsg("[ERROR] %s: failed to locate .text section", name);
        return nullptr;
    }

    const auto* base = reinterpret_cast<const uint8_t*>(hMod);
    const uint8_t* firstMatch = nullptr;
    bool matchedExpected = false;

    if (expectedRva != 0) {
        const uint8_t* cand = base + expectedRva;
        if (cand >= textStart && static_cast<size_t>(cand - textStart) <= textSize - pat.size()) {
            if (MatchPatternAt(cand, pat)) {
                matchedExpected = true;
            }
        }
    }

    const size_t scanLimit = textSize - pat.size();
    const uint8_t firstByte = pat[0].value;
    const bool firstWild = pat[0].wildcard;

    for (size_t i = 0; i <= scanLimit; ++i) {
        if (!firstWild && textStart[i] != firstByte) {
            continue;
        }
        if (MatchPatternAt(textStart + i, pat)) {
            ++outMatchCount;
            if (!firstMatch) {
                firstMatch = textStart + i;
            }
        }
    }

    if (outMatchCount == 0) {
        LogMsg("[ERROR] %s: pattern NOT FOUND in .text (expected RVA 0x%llX)",
               name, static_cast<unsigned long long>(expectedRva));
        return nullptr;
    }

    if (outMatchCount > 1) {
        LogMsg("[WARN] %s: matched %u times (expected RVA match=%d)",
               name, outMatchCount, matchedExpected ? 1 : 0);
        return matchedExpected ? (base + expectedRva) : nullptr;
    }

    uint64_t actualRva = static_cast<uint64_t>(firstMatch - base);
    if (expectedRva != 0 && actualRva != expectedRva) {
        LogMsg("[INFO] %s: relocated RVA 0x%llX (expected 0x%llX)",
               name,
               static_cast<unsigned long long>(actualRva),
               static_cast<unsigned long long>(expectedRva));
    }
    return firstMatch;
}

inline const uint8_t* ResolveRipRelative(const uint8_t* instrEnd, int32_t disp32) {
    return instrEnd + disp32;
}

inline bool HookImageImportIAT(
    HMODULE targetModule,
    const char* importDll,
    const char* funcName,
    void* hookFn,
    void** outOrigFn) {
    if (!targetModule || !importDll || !funcName || !hookFn) {
        return false;
    }
    auto* base = reinterpret_cast<uint8_t*>(targetModule);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
        return false;
    }
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE ||
        nt->FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64) {
        return false;
    }

    const IMAGE_DATA_DIRECTORY& impDir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (impDir.VirtualAddress == 0 || impDir.Size == 0) {
        return false;
    }

    const auto* desc = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(base + impDir.VirtualAddress);
    for (; desc->Name != 0; ++desc) {
        const char* modName = reinterpret_cast<const char*>(base + desc->Name);
        if (_stricmp(modName, importDll) != 0) {
            continue;
        }
        auto* ilt = reinterpret_cast<const IMAGE_THUNK_DATA64*>(
            base + (desc->OriginalFirstThunk ? desc->OriginalFirstThunk : desc->FirstThunk));
        auto* iat = reinterpret_cast<IMAGE_THUNK_DATA64*>(base + desc->FirstThunk);

        for (; ilt->u1.AddressOfData != 0; ++ilt, ++iat) {
            if (IMAGE_SNAP_BY_ORDINAL64(ilt->u1.Ordinal)) {
                continue;
            }
            const auto* byName = reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(
                base + ilt->u1.AddressOfData);
            if (strcmp(byName->Name, funcName) == 0) {
                if (outOrigFn) {
                    *outOrigFn = reinterpret_cast<void*>(static_cast<uintptr_t>(iat->u1.Function));
                }
                uint64_t newVal = reinterpret_cast<uint64_t>(hookFn);
                return WriteMem(&iat->u1.Function, &newVal, sizeof(newVal));
            }
        }
    }
    return false;
}

}  // namespace HookEngine
