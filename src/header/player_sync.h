#pragma once

#include "lua51.h"

namespace player_sync {
    void update(lua51::lua_State* state);
    void forget(lua51::lua_State* state);
}
