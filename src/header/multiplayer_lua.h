#pragma once

#include "lua51.h"

namespace multiplayer_lua {
    bool load(lua51::lua_State* state);
    void forget(lua51::lua_State* state);
}
