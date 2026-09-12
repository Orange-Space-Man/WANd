#include "lua51.h"

#include "memory.h"
#include "monitor.h"
#include "multiplayer_lua.h"
#include "noita.h"
#include "player_sync.h"
#include "remote_player.h"

#include <windows.h>

#include <cstddef>

namespace {
    using LuaLNewState = lua51::lua_State*(__cdecl*)();
    using LuaClose = void(__cdecl*)(lua51::lua_State*);
    using LuaPCall = int(__cdecl*)(lua51::lua_State*, int, int, int);
    using LuaGetTop = int(__cdecl*)(lua51::lua_State*);
    using LuaSetTop = void(__cdecl*)(lua51::lua_State*, int);
    using LuaCreateTable = void(__cdecl*)(lua51::lua_State*, int, int);
    using LuaGetField = void(__cdecl*)(lua51::lua_State*, int, const char*);
    using LuaSetField = void(__cdecl*)(lua51::lua_State*, int, const char*);
    using LuaRawGetIndex = void(__cdecl*)(lua51::lua_State*, int, int);
    using LuaPushString = void(__cdecl*)(lua51::lua_State*, const char*);
    using LuaPushNumber = void(__cdecl*)(lua51::lua_State*, double);
    using LuaPushBoolean = void(__cdecl*)(lua51::lua_State*, int);
    using LuaPushCClosure = void(__cdecl*)(lua51::lua_State*, lua51::LuaCFunction, int);
    using LuaType = int(__cdecl*)(lua51::lua_State*, int);
    using LuaToLString = const char*(__cdecl*)(lua51::lua_State*, int, std::size_t*);
    using LuaToNumber = double(__cdecl*)(lua51::lua_State*, int);
    using LuaToBoolean = int(__cdecl*)(lua51::lua_State*, int);

    void* volatile p_state = nullptr;
    LuaLNewState p_newState = nullptr;
    LuaClose p_close = nullptr;
    LuaPCall p_pcall = nullptr;
    LuaGetTop p_getTop = nullptr;
    LuaSetTop p_setTop = nullptr;
    LuaCreateTable p_createTable = nullptr;
    LuaGetField p_getField = nullptr;
    LuaSetField p_setField = nullptr;
    LuaRawGetIndex p_rawGetIndex = nullptr;
    LuaPushString p_pushString = nullptr;
    LuaPushNumber p_pushNumber = nullptr;
    LuaPushBoolean p_pushBoolean = nullptr;
    LuaPushCClosure p_pushCClosure = nullptr;
    LuaType p_type = nullptr;
    LuaToLString p_toLString = nullptr;
    LuaToNumber p_toNumber = nullptr;
    LuaToBoolean p_toBoolean = nullptr;

    template <typename T>
    T getExport(const char* name) {
        return reinterpret_cast<T>(GetProcAddress(noita::lua(), name));
    }

    void saveState(lua51::lua_State* state) {
        void* const previous = InterlockedCompareExchangePointer(&p_state, state, nullptr);
        if (previous == nullptr) {
            monitor::write("lua", "captured");
            monitor::write("log", "Lua state captured");
        }
    }

    lua51::lua_State* __cdecl hookNewState() {
        lua51::lua_State* const state = p_newState();
        saveState(state);
        return state;
    }

    void __cdecl hookClose(lua51::lua_State* state) {
        multiplayer_lua::forget(state);
        player_sync::forget(state);
        remote_player::forget(state);
        void* const previous = InterlockedCompareExchangePointer(&p_state, nullptr, state);
        if (previous == state) {
            monitor::write("lua", "waiting");
            monitor::write("log", "Lua state closed");
        }
        p_close(state);
    }

    int __cdecl hookPCall(lua51::lua_State* state, int arguments, int results, int errorFunction) {
        // EntityLoad and other engine APIs can call Lua again while replica
        // creation is in progress. Never run a second sync pass inside one:
        // its process-wide entity handles would overwrite the first hierarchy.
        static thread_local bool insideCall = false;
        if (insideCall) return p_pcall(state, arguments, results, errorFunction);
        struct CallScope {
            bool& active;
            explicit CallScope(bool& value) : active(value) { active = true; }
            ~CallScope() { active = false; }
        } scope(insideCall);
        saveState(state);
        multiplayer_lua::load(state);
        const int status = p_pcall(state, arguments, results, errorFunction);
        player_sync::update(state);
        remote_player::update(state);
        return status;
    }
}

bool lua51::init() {
    if (noita::game() == nullptr || noita::lua() == nullptr) {
        return false;
    }

    p_getTop = getExport<LuaGetTop>("lua_gettop");
    p_setTop = getExport<LuaSetTop>("lua_settop");
    p_createTable = getExport<LuaCreateTable>("lua_createtable");
    p_getField = getExport<LuaGetField>("lua_getfield");
    p_setField = getExport<LuaSetField>("lua_setfield");
    p_rawGetIndex = getExport<LuaRawGetIndex>("lua_rawgeti");
    p_pushString = getExport<LuaPushString>("lua_pushstring");
    p_pushNumber = getExport<LuaPushNumber>("lua_pushnumber");
    p_pushBoolean = getExport<LuaPushBoolean>("lua_pushboolean");
    p_pushCClosure = getExport<LuaPushCClosure>("lua_pushcclosure");
    p_type = getExport<LuaType>("lua_type");
    p_toLString = getExport<LuaToLString>("lua_tolstring");
    p_toNumber = getExport<LuaToNumber>("lua_tonumber");
    p_toBoolean = getExport<LuaToBoolean>("lua_toboolean");

    const bool newStateHooked = memory::hook_iat(noita::game(), "lua51.dll", "luaL_newstate", reinterpret_cast<void*>(&hookNewState), reinterpret_cast<void**>(&p_newState));
    const bool closeHooked = memory::hook_iat(noita::game(), "lua51.dll", "lua_close", reinterpret_cast<void*>(&hookClose), reinterpret_cast<void**>(&p_close));
    const bool pcallHooked = memory::hook_iat(noita::game(), "lua51.dll", "lua_pcall", reinterpret_cast<void*>(&hookPCall), reinterpret_cast<void**>(&p_pcall));
    if (!newStateHooked || !closeHooked || !pcallHooked) {
        OutputDebugStringA("[WANd] Lua state hooks failed.\n");
        monitor::write("lua", "hook failed");
        monitor::write("log", "Lua state hooks failed");
        return false;
    }

    OutputDebugStringA("[WANd] Lua state hooks installed.\n");
    monitor::write("lua", "waiting");
    monitor::write("log", "Lua state hooks installed");
    return true;
}

bool lua51::ready() {
    return p_newState != nullptr && p_close != nullptr && p_pcall != nullptr && p_getTop != nullptr && p_setTop != nullptr && p_createTable != nullptr && p_getField != nullptr && p_setField != nullptr && p_rawGetIndex != nullptr && p_pushString != nullptr && p_pushNumber != nullptr && p_pushBoolean != nullptr && p_pushCClosure != nullptr && p_type != nullptr && p_toLString != nullptr && p_toNumber != nullptr && p_toBoolean != nullptr;
}

lua51::lua_State* lua51::getState() {
    return static_cast<lua_State*>(InterlockedCompareExchangePointer(&p_state, nullptr, nullptr));
}

int lua51::getTop(lua_State* state) {
    return p_getTop(state);
}

void lua51::setTop(lua_State* state, int index) {
    p_setTop(state, index);
}

void lua51::createTable(lua_State* state, int arrayCount, int fieldCount) {
    p_createTable(state, arrayCount, fieldCount);
}

void lua51::getField(lua_State* state, int index, const char* name) {
    p_getField(state, index, name);
}

void lua51::setField(lua_State* state, int index, const char* name) {
    p_setField(state, index, name);
}

void lua51::rawGetIndex(lua_State* state, int index, int item) {
    p_rawGetIndex(state, index, item);
}

void lua51::getGlobal(lua_State* state, const char* name) {
    p_getField(state, globalsIndex, name);
}

void lua51::setGlobal(lua_State* state, const char* name) {
    p_setField(state, globalsIndex, name);
}

void lua51::pushString(lua_State* state, const char* value) {
    p_pushString(state, value);
}

void lua51::pushNumber(lua_State* state, double value) {
    p_pushNumber(state, value);
}

void lua51::pushBoolean(lua_State* state, bool value) {
    if (value) {
        p_pushBoolean(state, 1);
    } else {
        p_pushBoolean(state, 0);
    }
}

void lua51::pushFunction(lua_State* state, LuaCFunction function) {
    p_pushCClosure(state, function, 0);
}

int lua51::type(lua_State* state, int index) {
    return p_type(state, index);
}

const char* lua51::toString(lua_State* state, int index) {
    return p_toLString(state, index, nullptr);
}

double lua51::toNumber(lua_State* state, int index) {
    return p_toNumber(state, index);
}

bool lua51::toBoolean(lua_State* state, int index) {
    return p_toBoolean(state, index) != 0;
}

int lua51::pcall(lua_State* state, int arguments, int results, int errorFunction) {
    return p_pcall(state, arguments, results, errorFunction);
}
