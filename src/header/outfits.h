#pragma once
#include "lua51.h"
#include <cstdint>
#include <vector>

namespace outfits {
    constexpr unsigned count = 6;
    constexpr unsigned hideCape = 1U << 8, crown = 1U << 9, amulet = 1U << 10, gem = 1U << 11;
    struct Color { const char* name; unsigned char r, g, b; };
    struct Portrait { unsigned width = 0, height = 0; std::vector<unsigned char> rgba; };
    const Color& color(unsigned id);
    unsigned selected();
    void select(unsigned id);
    unsigned appearance();
    bool unlocked(unsigned accessory);
    bool enabled(unsigned option);
    void toggle(unsigned option);
    void refreshUnlocks(lua51::lua_State* state = nullptr);
    Portrait preview(unsigned appearance);
    void animate(lua51::lua_State* state, int entity, const char* animation);
    void recolor(unsigned char* rgba, unsigned id);
    void initialize();
    bool ready();
    bool failed();
    const Portrait* portrait(unsigned id);
    void forget(bool remote);
    void apply(lua51::lua_State* state, int entity, unsigned id, bool remote);
}
