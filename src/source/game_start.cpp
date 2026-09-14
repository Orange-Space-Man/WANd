#include "game_start.h"

#include "memory.h"
#include "monitor.h"
#include "network.h"
#include "multiplayer_menu.h"
#include "noita.h"

#include <windows.h>

#include <cstdint>
#include <cstring>

namespace {
    constexpr DWORD p_startTimeout = 5000;
    constexpr unsigned char p_startBytes[] = { 0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF8, 0x6A, 0xFF };
    constexpr unsigned char p_heartbeatBytes[] = { 0xC7, 0x84, 0x24, 0x38, 0x01, 0x00, 0x00, 0x0F, 0x00, 0x00, 0x00 };

    using StartNewGame = void(__fastcall*)(void*, void*);
    using SwapWindow = void(__cdecl*)(void*);

    StartNewGame p_startNewGame = nullptr;
    SwapWindow p_swapWindow = nullptr;
    void* volatile p_gameMode = nullptr;
    void* volatile p_startOptions = nullptr;
    volatile LONG p_hostStart = FALSE;
    volatile LONG p_clientStart = FALSE;
    volatile LONG p_requestedStart = FALSE;
    std::uint32_t p_seed = 0;
    DWORD p_startTime = 0;

    unsigned char* gameAddress(std::uintptr_t rva) {
        return reinterpret_cast<unsigned char*>(noita::game()) + rva;
    }

    std::uint32_t makeSeed() {
        LARGE_INTEGER counter{};
        QueryPerformanceCounter(&counter);
        std::uint32_t seed = static_cast<std::uint32_t>(counter.QuadPart);
        seed ^= static_cast<std::uint32_t>(counter.QuadPart >> 32);
        seed ^= GetTickCount();
        seed ^= GetCurrentProcessId() << 16;
        if (seed == 0) {
            seed = 1;
        }
        return seed;
    }

    bool setSeed(std::uint32_t seed) {
        const unsigned char enabled = 1;
        if (!memory::write(gameAddress(noita::worldSeedRva), &seed, sizeof(seed))) {
            return false;
        }
        return memory::write(gameAddress(noita::worldSeedOverrideFlagRva), &enabled, sizeof(enabled));
    }

    void startGame(void* gameMode, void* startOptions, std::uint32_t seed) {
        multiplayer_menu::closeForRun();
        if (!setSeed(seed)) {
            monitor::write("log", "Could not set shared world seed");
        }
        p_startNewGame(gameMode, startOptions);
    }

    void __fastcall startNewGame(void* gameMode, void* startOptions) {
        if (network::status() != network::Status::connected) {
            p_startNewGame(gameMode, startOptions);
            return;
        }

        if (!network::isHost()) {
            if (InterlockedExchange(&p_clientStart, FALSE) == TRUE) {
                monitor::write("log", "Client New Game intercepted");
                startGame(gameMode, startOptions, p_seed);
            } else {
                monitor::write("log", "Only the host can start a connected run");
            }
            return;
        }

        if (InterlockedCompareExchange(&p_hostStart, TRUE, FALSE) != FALSE) {
            return;
        }

        monitor::write("log", "Host New Game intercepted");
        p_seed = makeSeed();
        if (!network::beginRun(p_seed)) {
            InterlockedExchange(&p_hostStart, FALSE);
            p_startNewGame(gameMode, startOptions);
            return;
        }

        InterlockedExchangePointer(&p_gameMode, gameMode);
        InterlockedExchangePointer(&p_startOptions, startOptions);
        p_startTime = GetTickCount();
    }

    void __cdecl swapWindow(void* window) {
        game_start::update();
        multiplayer_menu::draw();
        p_swapWindow(window);
    }
}

extern "C" void __cdecl updateSharedStart() {
    game_start::update();
}

extern "C" __declspec(naked) void sharedStartFrame() {
    __asm {
        mov dword ptr [esp + 13Ch], 0Fh
        pushfd
        pushad
        call updateSharedStart
        popad
        popfd
        ret
    }
}

bool game_start::init() {
    if (noita::game() == nullptr) {
        return false;
    }

    unsigned char* const start = gameAddress(noita::finalNewGameStartRva);
    if (std::memcmp(start, p_startBytes, sizeof(p_startBytes)) != 0) {
        monitor::write("log", "New Game hook does not match this Noita build");
        return false;
    }

    if (!memory::hook(start, reinterpret_cast<void*>(&startNewGame), reinterpret_cast<void**>(&p_startNewGame))) {
        monitor::write("log", "New Game hook failed");
        return false;
    }

    if (!memory::hook_iat(noita::game(), "SDL2.dll", "SDL_GL_SwapWindow", reinterpret_cast<void*>(&swapWindow), reinterpret_cast<void**>(&p_swapWindow))) {
        monitor::write("log", "Shared New Game frame hook failed");
        return false;
    }

    unsigned char* const heartbeat = gameAddress(noita::mainMenuHeartbeatRva);
    if (std::memcmp(heartbeat, p_heartbeatBytes, sizeof(p_heartbeatBytes)) != 0 || !memory::call(heartbeat, reinterpret_cast<void*>(&sharedStartFrame), sizeof(p_heartbeatBytes))) {
        monitor::write("log", "Shared New Game menu heartbeat unavailable");
    }

    monitor::write("log", "Shared New Game hook installed");
    return true;
}

void game_start::update() {
    if (p_startNewGame == nullptr) {
        return;
    }

    if (InterlockedExchange(&p_requestedStart, FALSE) && network::isHost() && network::status() == network::Status::connected) {
        startNewGame(nullptr, nullptr);
    }

    std::uint32_t seed = 0;
    if (network::status() == network::Status::connected && !network::isHost() && network::takeRun(seed)) {
        p_seed = seed;
        InterlockedExchange(&p_clientStart, TRUE);
        monitor::write("log", "Starting shared client run");
        startNewGame(nullptr, nullptr);
        return;
    }

    if (InterlockedCompareExchange(&p_hostStart, FALSE, FALSE) == FALSE) {
        return;
    }

    const bool timedOut = GetTickCount() - p_startTime >= p_startTimeout;
    if (!network::runReady() && !timedOut && network::status() == network::Status::connected) {
        return;
    }

    if (timedOut) {
        monitor::write("log", "Player ready timed out, starting host run");
    }
    InterlockedExchange(&p_hostStart, FALSE);
    void* const gameMode = InterlockedExchangePointer(&p_gameMode, nullptr);
    void* const startOptions = InterlockedExchangePointer(&p_startOptions, nullptr);
    startGame(gameMode, startOptions, p_seed);
}

bool game_start::requestStart() {
    if (!p_startNewGame || !network::isHost() || network::status() != network::Status::connected) return false;
    InterlockedExchange(&p_requestedStart, TRUE);
    return true;
}
