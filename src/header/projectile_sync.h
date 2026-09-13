#pragma once
#include "lua51.h"
namespace projectile_sync {
    void update(lua51::lua_State* state);
    void forget();
}
