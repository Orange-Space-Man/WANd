#pragma once
#include "lua51.h"
#include "network.h"
namespace bomb_sync {
    void install(lua51::lua_State* state);
    bool track(lua51::lua_State* state, int entity, const network::ProjectileEvent& event);
    void receive(lua51::lua_State* state, const network::ProjectileEvent& event);
    void update(lua51::lua_State* state);
    void forget(lua51::lua_State* state);
}
