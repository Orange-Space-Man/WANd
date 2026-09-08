#include "player_sync.h"

#include "monitor.h"
#include "network.h"

#include <windows.h>

#include <cstdio>

namespace {
    constexpr DWORD p_updateTime = 50;
    void* volatile p_gameState = nullptr;
    LONG p_lastUpdate = 0;

    bool getPlayer(lua51::lua_State* state, int& player) {
        const int top = lua51::getTop(state);
        lua51::getGlobal(state, "EntityGetWithTag");
        if (lua51::type(state, -1) != lua51::typeFunction) {
            lua51::setTop(state, top);
            return false;
        }

        lua51::pushString(state, "player_unit");
        if (lua51::pcall(state, 1, 1, 0) != 0 || lua51::type(state, -1) != lua51::typeTable) {
            lua51::setTop(state, top);
            return false;
        }

        lua51::rawGetIndex(state, -1, 1);
        if (lua51::type(state, -1) != lua51::typeNumber) {
            lua51::setTop(state, top);
            return false;
        }

        player = static_cast<int>(lua51::toNumber(state, -1));
        lua51::setTop(state, top);
        return player > 0;
    }

    bool getTransform(lua51::lua_State* state, int player, float& x, float& y) {
        const int top = lua51::getTop(state);
        lua51::getGlobal(state, "EntityGetTransform");
        if (lua51::type(state, -1) != lua51::typeFunction) {
            lua51::setTop(state, top);
            return false;
        }

        lua51::pushNumber(state, player);
        if (lua51::pcall(state, 1, 2, 0) != 0 || lua51::type(state, -2) != lua51::typeNumber || lua51::type(state, -1) != lua51::typeNumber) {
            lua51::setTop(state, top);
            return false;
        }

        x = static_cast<float>(lua51::toNumber(state, -2));
        y = static_cast<float>(lua51::toNumber(state, -1));
        lua51::setTop(state, top);
        return true;
    }

    void getVelocity(lua51::lua_State* state, int player, float& x, float& y) {
        const int top = lua51::getTop(state);
        lua51::getGlobal(state, "GameGetVelocityCompVelocity");
        if (lua51::type(state, -1) == lua51::typeFunction) {
            lua51::pushNumber(state, player);
            if (lua51::pcall(state, 1, 2, 0) == 0 && lua51::type(state, -2) == lua51::typeNumber && lua51::type(state, -1) == lua51::typeNumber) {
                x = static_cast<float>(lua51::toNumber(state, -2));
                y = static_cast<float>(lua51::toNumber(state, -1));
            }
        }
        lua51::setTop(state, top);
    }

    int getControls(lua51::lua_State* state, int player) {
        const int top = lua51::getTop(state);
        lua51::getGlobal(state, "EntityGetFirstComponentIncludingDisabled");
        if (lua51::type(state, -1) != lua51::typeFunction) {
            lua51::setTop(state, top);
            return 0;
        }

        lua51::pushNumber(state, player);
        lua51::pushString(state, "ControlsComponent");
        if (lua51::pcall(state, 2, 1, 0) != 0 || lua51::type(state, -1) != lua51::typeNumber) {
            lua51::setTop(state, top);
            return 0;
        }

        const int component = static_cast<int>(lua51::toNumber(state, -1));
        lua51::setTop(state, top);
        return component;
    }

    void getAim(lua51::lua_State* state, int component, float& x, float& y) {
        if (component == 0) {
            return;
        }

        const int top = lua51::getTop(state);
        lua51::getGlobal(state, "ComponentGetValue2");
        if (lua51::type(state, -1) == lua51::typeFunction) {
            lua51::pushNumber(state, component);
            lua51::pushString(state, "mAimingVectorNormalized");
            if (lua51::pcall(state, 2, 2, 0) == 0 && lua51::type(state, -2) == lua51::typeNumber && lua51::type(state, -1) == lua51::typeNumber) {
                x = static_cast<float>(lua51::toNumber(state, -2));
                y = static_cast<float>(lua51::toNumber(state, -1));
            }
        }
        lua51::setTop(state, top);
    }
}

void player_sync::update(lua51::lua_State* state) {
    if (state == nullptr || !lua51::ready()) {
        return;
    }

    void* const gameState = InterlockedCompareExchangePointer(&p_gameState, nullptr, nullptr);
    if (gameState != nullptr && gameState != state) {
        return;
    }

    const DWORD now = GetTickCount();
    const DWORD previous = static_cast<DWORD>(InterlockedCompareExchange(&p_lastUpdate, 0, 0));
    if (now - previous < p_updateTime) {
        return;
    }
    InterlockedExchange(&p_lastUpdate, static_cast<LONG>(now));

    int player = 0;
    if (!getPlayer(state, player)) {
        return;
    }
    if (gameState == nullptr) {
        InterlockedExchangePointer(&p_gameState, state);
        monitor::write("log", "Local player found");
    }

    float x = 0.0f;
    float y = 0.0f;
    if (!getTransform(state, player, x, y)) {
        return;
    }

    float velocityX = 0.0f;
    float velocityY = 0.0f;
    getVelocity(state, player, velocityX, velocityY);
    float aimX = 0.0f;
    float aimY = 0.0f;
    getAim(state, getControls(state, player), aimX, aimY);

    const char* facing = "right";
    if (aimX < 0.0f) {
        facing = "left";
    }

    network::PlayerState playerState{};
    playerState.x = x;
    playerState.y = y;
    playerState.velocityX = velocityX;
    playerState.velocityY = velocityY;
    playerState.aimX = aimX;
    playerState.aimY = aimY;
    playerState.facingLeft = aimX < 0.0f;
    network::sendPlayer(playerState);

    char text[256]{};
    _snprintf_s(text, sizeof(text), _TRUNCATE, "%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%s", x, y, velocityX, velocityY, aimX, aimY, facing);
    monitor::write("player", text);
}

void player_sync::forget(lua51::lua_State* state) {
    InterlockedCompareExchangePointer(&p_gameState, nullptr, state);
}
