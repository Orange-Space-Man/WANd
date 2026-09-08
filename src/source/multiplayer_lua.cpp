#include "multiplayer_lua.h"

#include "monitor.h"
#include "network.h"

#include <windows.h>

namespace {
    constexpr unsigned short p_defaultPort = 27888;
    void* volatile p_loadedState = nullptr;

    int luaHost(lua51::lua_State* state) {
        unsigned short port = p_defaultPort;
        if (lua51::getTop(state) >= 1 && lua51::type(state, 1) == lua51::typeNumber) {
            const double value = lua51::toNumber(state, 1);
            if (value >= 1.0 && value <= 65535.0) {
                port = static_cast<unsigned short>(value);
            }
        }

        lua51::pushBoolean(state, network::host(port));
        return 1;
    }

    int luaJoin(lua51::lua_State* state) {
        const char* address = nullptr;
        if (lua51::getTop(state) >= 1 && lua51::type(state, 1) == lua51::typeString) {
            address = lua51::toString(state, 1);
        }

        unsigned short port = p_defaultPort;
        if (lua51::getTop(state) >= 2 && lua51::type(state, 2) == lua51::typeNumber) {
            const double value = lua51::toNumber(state, 2);
            if (value >= 1.0 && value <= 65535.0) {
                port = static_cast<unsigned short>(value);
            }
        }

        lua51::pushBoolean(state, network::join(address, port));
        return 1;
    }

    int luaDisconnect(lua51::lua_State*) {
        network::stop();
        return 0;
    }

    int luaGetStatus(lua51::lua_State* state) {
        lua51::pushString(state, network::statusText());
        return 1;
    }

    int luaIsHost(lua51::lua_State* state) {
        lua51::pushBoolean(state, network::isHost());
        return 1;
    }
}

bool multiplayer_lua::load(lua51::lua_State* state) {
    if (state == nullptr || !lua51::ready()) {
        return false;
    }
    if (InterlockedCompareExchangePointer(&p_loadedState, nullptr, nullptr) == state) {
        return true;
    }

    const int top = lua51::getTop(state);
    lua51::getGlobal(state, "network");
    if (lua51::type(state, -1) != lua51::typeTable) {
        lua51::setTop(state, top);
        lua51::createTable(state, 0, 5);
    }

    lua51::pushFunction(state, luaHost);
    lua51::setField(state, -2, "Host");
    lua51::pushFunction(state, luaJoin);
    lua51::setField(state, -2, "Join");
    lua51::pushFunction(state, luaDisconnect);
    lua51::setField(state, -2, "Disconnect");
    lua51::pushFunction(state, luaGetStatus);
    lua51::setField(state, -2, "GetStatus");
    lua51::pushFunction(state, luaIsHost);
    lua51::setField(state, -2, "IsHost");
    lua51::setGlobal(state, "network");
    lua51::setTop(state, top);

    InterlockedExchangePointer(&p_loadedState, state);
    monitor::write("log", "network Lua API installed");
    return true;
}

void multiplayer_lua::forget(lua51::lua_State* state) {
    InterlockedCompareExchangePointer(&p_loadedState, nullptr, state);
}
