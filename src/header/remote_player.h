#pragma once

#include "lua51.h"

namespace remote_player {
    void update(lua51::lua_State* state);
    void forget(lua51::lua_State* state);
}
