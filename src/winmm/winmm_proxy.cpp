#include <windows.h>

#include <cwchar>

namespace {
    constexpr UINT p_ordinalBase = 2;
    constexpr UINT p_exportCount = 193;

    INIT_ONCE p_loaded = INIT_ONCE_STATIC_INIT;
    HMODULE p_winmm = nullptr;
    FARPROC p_exports[p_exportCount]{};

    BOOL CALLBACK loadWinmm(PINIT_ONCE, PVOID, PVOID*) {
        wchar_t path[MAX_PATH]{};
        const UINT length = GetSystemDirectoryW(path, MAX_PATH);
        if (length == 0 || length >= MAX_PATH || wcscat_s(path, L"\\winmm.dll") != 0) {
            return TRUE;
        }

        p_winmm = LoadLibraryW(path);
        return TRUE;
    }
}

extern "C" void __stdcall ResolveWinmmExport(UINT ordinal) {
    if (ordinal < p_ordinalBase || ordinal >= p_ordinalBase + p_exportCount) {
        return;
    }

    void* volatile* const slot = reinterpret_cast<void* volatile*>(&p_exports[ordinal - p_ordinalBase]);
    if (InterlockedCompareExchangePointer(slot, nullptr, nullptr) != nullptr) {
        return;
    }

    InitOnceExecuteOnce(&p_loaded, loadWinmm, nullptr, nullptr);
    if (p_winmm == nullptr) {
        return;
    }

    void* const address = reinterpret_cast<void*>(GetProcAddress(p_winmm, MAKEINTRESOURCEA(ordinal)));
    InterlockedCompareExchangePointer(slot, address, nullptr);
}

#define WINMM_PROXY(ordinal, offset)                         \
    extern "C" __declspec(naked) void ProxyWinmm##ordinal() \
    {                                                        \
        __asm pushfd                                         \
        __asm pushad                                         \
        __asm push ordinal                                   \
        __asm call ResolveWinmmExport                        \
        __asm popad                                          \
        __asm popfd                                          \
        __asm jmp dword ptr [p_exports + offset]             \
    }

#include "winmm_exports.inc"

#undef WINMM_PROXY
