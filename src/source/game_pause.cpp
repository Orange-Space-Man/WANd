#include "game_pause.h"

#include "game_start.h"
#include "memory.h"
#include "monitor.h"
#include "network.h"
#include "noita.h"

#include <windows.h>

namespace {
    constexpr unsigned int p_windowEvent = 0x200;
    constexpr unsigned char p_focusLost = 13;

    struct GameGlobal {
        int frame;
        char unknown1[8];
        void* gameWorld;
        void* gridWorld;
        void* textures;
        void* cellFactory;
        char unknown2[44];
        int* pauseState;
    };

    struct WindowEvent {
        unsigned int type;
        unsigned int timestamp;
        unsigned int window;
        unsigned char event;
        unsigned char padding1;
        unsigned char padding2;
        unsigned char padding3;
        int data1;
        int data2;
    };

    using GetGameGlobal = GameGlobal* (__stdcall*)();
    using GameSimulate = void(__thiscall*)(void*, float);
    using PollEvent = int(__cdecl*)(void*);

    GetGameGlobal p_getGameGlobal = nullptr;
    GameSimulate p_gameSimulate = nullptr;
    PollEvent p_pollEvent = nullptr;

    bool connected() {
        return network::status() == network::Status::connected;
    }

    int __cdecl pollEvent(void* event) {
        game_start::update();
        int result = p_pollEvent(event);
        while (result != 0 && event != nullptr && connected()) {
            const WindowEvent* const windowEvent = static_cast<const WindowEvent*>(event);
            if (windowEvent->type != p_windowEvent || windowEvent->event != p_focusLost) {
                break;
            }
            result = p_pollEvent(event);
        }
        return result;
    }

    void __fastcall gameSimulate(void* game, void*, float delta) {
        p_gameSimulate(game, delta);
        if (!connected() || p_getGameGlobal == nullptr) {
            return;
        }

        GameGlobal* const global = p_getGameGlobal();
        if (global == nullptr || global->pauseState == nullptr || *global->pauseState <= 0) {
            return;
        }

        const int pauseState = *global->pauseState;
        *global->pauseState = 0;
        p_gameSimulate(game, delta);
        *global->pauseState = pauseState;
    }

    bool hookFocus() {
        return memory::hook_iat(noita::game(), "SDL2.dll", "SDL_PollEvent", reinterpret_cast<void*>(&pollEvent), reinterpret_cast<void**>(&p_pollEvent));
    }

    bool hookSimulation() {
        void* const gameGlobalCall = memory::find(noita::game(), noita::gameGlobalPattern, noita::gameGlobalMask);
        p_getGameGlobal = reinterpret_cast<GetGameGlobal>(memory::relativeTarget(gameGlobalCall));
        if (p_getGameGlobal == nullptr) {
            return false;
        }

        void* const anchor = memory::findString(noita::game(), noita::pauseAnchor);
        void* const reference = memory::findReference(noita::game(), anchor);
        void* const simulate = memory::functionStart(noita::game(), reference);
        if (simulate == nullptr) {
            return false;
        }

        return memory::hook(simulate, reinterpret_cast<void*>(&gameSimulate), reinterpret_cast<void**>(&p_gameSimulate));
    }
}

bool game_pause::init() {
    const bool focus = hookFocus();
    const bool simulation = hookSimulation();
    if (focus) {
        monitor::write("log", "Connected sessions ignore focus loss");
    } else {
        monitor::write("log", "Focus pause hook failed");
    }
    if (simulation) {
        monitor::write("log", "Connected sessions keep simulation running");
    } else {
        monitor::write("log", "Simulation pause hook failed");
    }
    return focus && simulation;
}
