#include "multiplayer_entry.h"

#include "control.h"
#include "game_pause.h"
#include "game_start.h"
#include "lua51.h"
#include "monitor.h"
#include "network.h"
#include "noita.h"

#include <windows.h>

#include <cstdio>

namespace {
    INIT_ONCE p_started = INIT_ONCE_STATIC_INIT;

    DWORD WINAPI hookNoita(LPVOID) {
        if (noita::init()) {
            game_start::init();
            game_pause::init();
            lua51::init();
        } else {
            monitor::write("lua", "lua51.dll not found");
        }
        return 0;
    }

    DWORD WINAPI updateMonitor(LPVOID) {
        for (;;) {
            char controlPort[16]{};
            _snprintf_s(controlPort, sizeof(controlPort), _TRUNCATE, "%hu", control::port());
            monitor::write("control", controlPort);
            monitor::write("role", network::role());
            monitor::write("state", network::statusText());
            if (lua51::getState() != nullptr) {
                monitor::write("lua", "captured");
            } else if (lua51::ready()) {
                monitor::write("lua", "waiting");
            } else {
                monitor::write("lua", "hooking");
            }
            Sleep(1000);
        }
    }

    BOOL CALLBACK startMultiplayer(PINIT_ONCE, PVOID, PVOID*) {
        network::init();
        monitor::init();
        control::init();
        monitor::write("log", "WANd initialized");
        const HANDLE thread = CreateThread(nullptr, 0, hookNoita, nullptr, 0, nullptr);
        if (thread != nullptr) {
            CloseHandle(thread);
        }
        const HANDLE monitorThread = CreateThread(nullptr, 0, updateMonitor, nullptr, 0, nullptr);
        if (monitorThread != nullptr) {
            CloseHandle(monitorThread);
        }
        OutputDebugStringA("[WANd] Multiplayer initialized.\n");
        return TRUE;
    }
}

extern "C" void __cdecl WANdStart() {
    InitOnceExecuteOnce(&p_started, startMultiplayer, nullptr, nullptr);
}

extern "C" int __cdecl WANdHost(unsigned short port) {
    WANdStart();
    if (network::host(port)) {
        return TRUE;
    }
    return FALSE;
}

extern "C" int __cdecl WANdJoin(const char* address, unsigned short port) {
    WANdStart();
    if (network::join(address, port)) {
        return TRUE;
    }
    return FALSE;
}

extern "C" void __cdecl WANdStop() {
    network::stop();
}

extern "C" int __cdecl WANdStatus() { 
    return static_cast<int>(network::status());
}

extern "C" void* __cdecl WANdLuaState() {
    return lua51::getState();
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
    }
    return TRUE;
}
