#include <windows.h>

#include <cwchar>

namespace {
    using WANdStart = void(__cdecl*)();

    DWORD WINAPI loadWANd(LPVOID parameter) {
        const HMODULE proxy = static_cast<HMODULE>(parameter);
        wchar_t path[MAX_PATH]{};
        const DWORD length = GetModuleFileNameW(proxy, path, MAX_PATH);
        if (length == 0 || length >= MAX_PATH) {
            OutputDebugStringA("[WANd] Could not find the proxy path.\n");
            return 0;
        }

        wchar_t* const filename = std::wcsrchr(path, L'\\');
        if (filename == nullptr) {
            OutputDebugStringA("[WANd] Could not find the proxy directory.\n");
            return 0;
        }
        filename[1] = L'\0';
        if (wcscat_s(path, L"WANd.dll") != 0) {
            OutputDebugStringA("[WANd] Could not create the WANd.dll path.\n");
            return 0;
        }

        const HMODULE multiplayer = LoadLibraryW(path);
        if (multiplayer == nullptr) {
            OutputDebugStringA("[WANd] Could not load WANd.dll.\n");
            return 0;
        }

        WANdStart start = reinterpret_cast<WANdStart>(GetProcAddress(multiplayer, "WANdStart"));
        if (start == nullptr) {
            OutputDebugStringA("[WANd] WANdStart was not found.\n");
            return 0;
        }

        start();
        return 0;
    }
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
        const HANDLE thread = CreateThread(nullptr, 0, loadWANd, module, 0, nullptr);
        if (thread != nullptr) {
            CloseHandle(thread);
        }
    }
    return TRUE;
}
