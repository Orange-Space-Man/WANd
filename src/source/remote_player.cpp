#include "remote_player.h"

#include "monitor.h"
#include "network.h"

#include <windows.h>

namespace {
    constexpr DWORD p_updateTime = 16;
    void* volatile p_gameState = nullptr;
    int p_entity = 0;
    int p_sprite = 0;
    float p_x = 0.0f;
    float p_y = 0.0f;
    bool p_facingLeft = false;
    bool p_hasFacing = false;
    DWORD p_lastUpdate = 0;

    bool beginCall(lua51::lua_State* state, const char* name, int top) {
        lua51::getGlobal(state, name);
        if (lua51::type(state, -1) == lua51::typeFunction) {
            return true;
        }
        lua51::setTop(state, top);
        return false;
    }

    void killRemote(lua51::lua_State* state) {
        if (p_entity == 0) {
            return;
        }

        const int top = lua51::getTop(state);
        if (beginCall(state, "EntityKill", top)) {
            lua51::pushNumber(state, p_entity);
            lua51::pcall(state, 1, 0, 0);
        }
        lua51::setTop(state, top);
        p_entity = 0;
        p_sprite = 0;
        p_hasFacing = false;
        monitor::write("log", "Remote player removed");
    }

    int createEntity(lua51::lua_State* state, const network::PlayerState& player) {
        const int top = lua51::getTop(state);
        if (!beginCall(state, "EntityCreateNew", top)) {
            return 0;
        }
        lua51::pushString(state, "WANd remote player");
        if (lua51::pcall(state, 1, 1, 0) != 0 || lua51::type(state, -1) != lua51::typeNumber) {
            lua51::setTop(state, top);
            return 0;
        }
        const int entity = static_cast<int>(lua51::toNumber(state, -1));
        lua51::setTop(state, top);
        if (entity == 0) {
            return 0;
        }

        if (beginCall(state, "EntityAddTag", top)) {
            lua51::pushNumber(state, entity);
            lua51::pushString(state, "wand_remote_player");
            lua51::pcall(state, 2, 0, 0);
        }
        lua51::setTop(state, top);

        if (!beginCall(state, "EntityAddComponent2", top)) {
            return entity;
        }
        lua51::pushNumber(state, entity);
        lua51::pushString(state, "SpriteComponent");
        lua51::createTable(state, 0, 9);
        lua51::pushString(state, "wand_remote_player");
        lua51::setField(state, -2, "_tags");
        lua51::pushString(state, "data/enemies_gfx/player.xml");
        lua51::setField(state, -2, "image_file");
        lua51::pushString(state, "stand");
        lua51::setField(state, -2, "rect_animation");
        lua51::pushNumber(state, 6.0);
        lua51::setField(state, -2, "offset_x");
        lua51::pushNumber(state, 14.0);
        lua51::setField(state, -2, "offset_y");
        lua51::pushNumber(state, 0.9);
        lua51::setField(state, -2, "z_index");
        lua51::pushBoolean(state, true);
        lua51::setField(state, -2, "has_special_scale");
        float scaleX = 1.0f;
        if (player.facingLeft) {
            scaleX = -1.0f;
        }
        lua51::pushNumber(state, scaleX);
        lua51::setField(state, -2, "special_scale_x");
        lua51::pushNumber(state, 1.0);
        lua51::setField(state, -2, "special_scale_y");
        if (lua51::pcall(state, 3, 1, 0) == 0 && lua51::type(state, -1) == lua51::typeNumber) {
            p_sprite = static_cast<int>(lua51::toNumber(state, -1));
        }
        lua51::setTop(state, top);
        return entity;
    }

    void setTransform(lua51::lua_State* state, int entity, float x, float y) {
        const int top = lua51::getTop(state);
        if (beginCall(state, "EntitySetTransform", top)) {
            lua51::pushNumber(state, entity);
            lua51::pushNumber(state, x);
            lua51::pushNumber(state, y);
            lua51::pcall(state, 3, 0, 0);
        }
        lua51::setTop(state, top);
    }

    void setFacing(lua51::lua_State* state, bool facingLeft) {
        if (p_sprite == 0 || p_hasFacing && p_facingLeft == facingLeft) {
            return;
        }

        const int top = lua51::getTop(state);
        if (beginCall(state, "ComponentSetValue2", top)) {
            lua51::pushNumber(state, p_sprite);
            lua51::pushString(state, "special_scale_x");
            float scaleX = 1.0f;
            if (facingLeft) {
                scaleX = -1.0f;
            }
            lua51::pushNumber(state, scaleX);
            lua51::pcall(state, 3, 0, 0);
        }
        lua51::setTop(state, top);
        p_facingLeft = facingLeft;
        p_hasFacing = true;
    }
}

void remote_player::update(lua51::lua_State* state) {
    if (state == nullptr || !lua51::ready()) {
        return;
    }

    void* const gameState = InterlockedCompareExchangePointer(&p_gameState, nullptr, nullptr);
    if (gameState != nullptr && gameState != state) {
        return;
    }

    network::PlayerState player{};
    if (network::status() != network::Status::connected || !network::getRemotePlayer(player)) {
        if (gameState == state) {
            killRemote(state);
        }
        return;
    }

    const DWORD now = GetTickCount();
    if (now - p_lastUpdate < p_updateTime) {
        return;
    }
    p_lastUpdate = now;

    if (gameState == nullptr) {
        InterlockedExchangePointer(&p_gameState, state);
    }
    if (p_entity == 0) {
        p_entity = createEntity(state, player);
        if (p_entity == 0 || p_sprite == 0) {
            killRemote(state);
            return;
        }
        p_x = player.x;
        p_y = player.y;
        p_hasFacing = false;
        monitor::write("log", "Remote player created");
    } else {
        p_x += (player.x - p_x) * 0.35f;
        p_y += (player.y - p_y) * 0.35f;
    }

    setTransform(state, p_entity, p_x, p_y);
    setFacing(state, player.facingLeft);
}

void remote_player::forget(lua51::lua_State* state) {
    void* const gameState = InterlockedCompareExchangePointer(&p_gameState, nullptr, nullptr);
    if (gameState == state) {
        killRemote(state);
        InterlockedExchangePointer(&p_gameState, nullptr);
        p_lastUpdate = 0;
    }
}
