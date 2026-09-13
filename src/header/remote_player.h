#pragma once

#include "lua51.h"

namespace remote_player {
    bool wandTip(lua51::lua_State* state, float& x, float& y);
    void update(lua51::lua_State* state);
    void forget(lua51::lua_State* state);
}
