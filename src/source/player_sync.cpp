#include "player_sync.h"

#include "monitor.h"
#include "network.h"

#include <windows.h>

#include <cmath>
#include <cstdio>
#include <cstring>

namespace {
    constexpr DWORD p_updateTime = 50;
    LONG p_lastUpdate = 0;
    LONG p_foundPlayer = FALSE;

    bool getPlayer(lua51::lua_State* state, int& player) {
        const int top = lua51::getTop(state);
        lua51::getGlobal(state, "EntityGetWithTag");
        if (lua51::type(state, -1) != lua51::typeFunction) {
            lua51::setTop(state, top);
            return false;
        }

        lua51::pushString(state, "player_unit");
        if (lua51::pcall(state, 1, 1, 0) != 0 || lua51::type(state, -1) != lua51::typeTable) {
            lua51::setTop(state, top);
            return false;
        }

        lua51::rawGetIndex(state, -1, 1);
        if (lua51::type(state, -1) != lua51::typeNumber) {
            lua51::setTop(state, top);
            return false;
        }

        player = static_cast<int>(lua51::toNumber(state, -1));
        lua51::setTop(state, top);
        return player > 0;
    }

    bool getTransform(lua51::lua_State* state, int player, float& x, float& y) {
        const int top = lua51::getTop(state);
        lua51::getGlobal(state, "EntityGetTransform");
        if (lua51::type(state, -1) != lua51::typeFunction) {
            lua51::setTop(state, top);
            return false;
        }

        lua51::pushNumber(state, player);
        if (lua51::pcall(state, 1, 2, 0) != 0 || lua51::type(state, -2) != lua51::typeNumber || lua51::type(state, -1) != lua51::typeNumber) {
            lua51::setTop(state, top);
            return false;
        }

        x = static_cast<float>(lua51::toNumber(state, -2));
        y = static_cast<float>(lua51::toNumber(state, -1));
        lua51::setTop(state, top);
        return true;
    }

    bool getFullTransform(lua51::lua_State* state, int entity, float& x, float& y, float& rotation, float& scaleX, float& scaleY) {
        const int top = lua51::getTop(state);
        lua51::getGlobal(state, "EntityGetTransform");
        if (lua51::type(state, -1) != lua51::typeFunction) {
            lua51::setTop(state, top);
            return false;
        }

        lua51::pushNumber(state, entity);
        if (lua51::pcall(state, 1, 5, 0) != 0) {
            lua51::setTop(state, top);
            return false;
        }
        for (int index = -5; index <= -1; ++index) {
            if (lua51::type(state, index) != lua51::typeNumber) {
                lua51::setTop(state, top);
                return false;
            }
        }
        x = static_cast<float>(lua51::toNumber(state, -5));
        y = static_cast<float>(lua51::toNumber(state, -4));
        rotation = static_cast<float>(lua51::toNumber(state, -3));
        scaleX = static_cast<float>(lua51::toNumber(state, -2));
        scaleY = static_cast<float>(lua51::toNumber(state, -1));
        lua51::setTop(state, top);
        return true;
    }

    bool getWorldHotspot(lua51::lua_State* state, int entity, const char* tag, float& x, float& y) {
        const int top = lua51::getTop(state);
        lua51::getGlobal(state, "EntityGetHotspot");
        if (lua51::type(state, -1) != lua51::typeFunction) {
            lua51::setTop(state, top);
            return false;
        }
        lua51::pushNumber(state, entity);
        lua51::pushString(state, tag);
        lua51::pushBoolean(state, true);
        if (lua51::pcall(state, 3, 2, 0) != 0
            || lua51::type(state, -2) != lua51::typeNumber
            || lua51::type(state, -1) != lua51::typeNumber) {
            lua51::setTop(state, top);
            return false;
        }
        x = static_cast<float>(lua51::toNumber(state, -2));
        y = static_cast<float>(lua51::toNumber(state, -1));
        lua51::setTop(state, top);
        return true;
    }

    void getVelocity(lua51::lua_State* state, int player, float& x, float& y) {
        const int top = lua51::getTop(state);
        lua51::getGlobal(state, "GameGetVelocityCompVelocity");
        if (lua51::type(state, -1) == lua51::typeFunction) {
            lua51::pushNumber(state, player);
            if (lua51::pcall(state, 1, 2, 0) == 0 && lua51::type(state, -2) == lua51::typeNumber && lua51::type(state, -1) == lua51::typeNumber) {
                x = static_cast<float>(lua51::toNumber(state, -2));
                y = static_cast<float>(lua51::toNumber(state, -1));
            }
        }
        lua51::setTop(state, top);
    }

    int getControls(lua51::lua_State* state, int player) {
        const int top = lua51::getTop(state);
        lua51::getGlobal(state, "EntityGetFirstComponentIncludingDisabled");
        if (lua51::type(state, -1) != lua51::typeFunction) {
            lua51::setTop(state, top);
            return 0;
        }

        lua51::pushNumber(state, player);
        lua51::pushString(state, "ControlsComponent");
        if (lua51::pcall(state, 2, 1, 0) != 0 || lua51::type(state, -1) != lua51::typeNumber) {
            lua51::setTop(state, top);
            return 0;
        }

        const int component = static_cast<int>(lua51::toNumber(state, -1));
        lua51::setTop(state, top);
        return component;
    }

    void getAim(lua51::lua_State* state, int component, float& x, float& y) {
        if (component == 0) {
            return;
        }

        const int top = lua51::getTop(state);
        lua51::getGlobal(state, "ComponentGetValue2");
        if (lua51::type(state, -1) == lua51::typeFunction) {
            lua51::pushNumber(state, component);
            lua51::pushString(state, "mAimingVectorNormalized");
            if (lua51::pcall(state, 2, 2, 0) == 0 && lua51::type(state, -2) == lua51::typeNumber && lua51::type(state, -1) == lua51::typeNumber) {
                x = static_cast<float>(lua51::toNumber(state, -2));
                y = static_cast<float>(lua51::toNumber(state, -1));
            }
        }
        lua51::setTop(state, top);
    }

    bool getComponentBool(lua51::lua_State* state, int entity, const char* componentType, const char* field) {
        const int top = lua51::getTop(state);
        lua51::getGlobal(state, "EntityGetFirstComponentIncludingDisabled");
        if (lua51::type(state, -1) != lua51::typeFunction) {
            lua51::setTop(state, top);
            return false;
        }

        lua51::pushNumber(state, entity);
        lua51::pushString(state, componentType);
        if (lua51::pcall(state, 2, 1, 0) != 0 || lua51::type(state, -1) != lua51::typeNumber) {
            lua51::setTop(state, top);
            return false;
        }

        const int component = static_cast<int>(lua51::toNumber(state, -1));
        lua51::setTop(state, top);
        lua51::getGlobal(state, "ComponentGetValue2");
        if (lua51::type(state, -1) != lua51::typeFunction) {
            lua51::setTop(state, top);
            return false;
        }

        lua51::pushNumber(state, component);
        lua51::pushString(state, field);
        if (lua51::pcall(state, 2, 1, 0) != 0 || lua51::type(state, -1) != lua51::typeBoolean) {
            lua51::setTop(state, top);
            return false;
        }

        const bool value = lua51::toBoolean(state, -1);
        lua51::setTop(state, top);
        return value;
    }

    int getComponent(lua51::lua_State* state, int entity, const char* componentType) {
        const int top = lua51::getTop(state);
        lua51::getGlobal(state, "EntityGetFirstComponentIncludingDisabled");
        if (lua51::type(state, -1) != lua51::typeFunction) {
            lua51::setTop(state, top);
            return 0;
        }

        lua51::pushNumber(state, entity);
        lua51::pushString(state, componentType);
        if (lua51::pcall(state, 2, 1, 0) != 0 || lua51::type(state, -1) != lua51::typeNumber) {
            lua51::setTop(state, top);
            return 0;
        }

        const int component = static_cast<int>(lua51::toNumber(state, -1));
        lua51::setTop(state, top);
        return component;
    }

    int getChild(lua51::lua_State* state, int entity, const char* tag) {
        const int top = lua51::getTop(state);
        lua51::getGlobal(state, "EntityGetAllChildren");
        if (lua51::type(state, -1) != lua51::typeFunction) {
            lua51::setTop(state, top);
            return 0;
        }
        lua51::pushNumber(state, entity);
        if (lua51::pcall(state, 1, 1, 0) != 0 || lua51::type(state, -1) != lua51::typeTable) {
            lua51::setTop(state, top);
            return 0;
        }

        int child = 0;
        for (int index = 1; index <= 64; ++index) {
            lua51::rawGetIndex(state, -1, index);
            if (lua51::type(state, -1) != lua51::typeNumber) {
                lua51::setTop(state, top + 1);
                break;
            }
            const int candidate = static_cast<int>(lua51::toNumber(state, -1));
            lua51::setTop(state, top + 1);
            lua51::getGlobal(state, "EntityHasTag");
            if (lua51::type(state, -1) == lua51::typeFunction) {
                lua51::pushNumber(state, candidate);
                lua51::pushString(state, tag);
                if (lua51::pcall(state, 2, 1, 0) == 0
                    && lua51::type(state, -1) == lua51::typeBoolean
                    && lua51::toBoolean(state, -1)) {
                    child = candidate;
                    lua51::setTop(state, top + 1);
                    break;
                }
            }
            lua51::setTop(state, top + 1);
        }
        lua51::setTop(state, top);
        return child;
    }

    int getTaggedComponent(lua51::lua_State* state, int entity, const char* componentType, const char* tag) {
        const int top = lua51::getTop(state);
        lua51::getGlobal(state, "EntityGetFirstComponentIncludingDisabled");
        if (lua51::type(state, -1) != lua51::typeFunction) {
            lua51::setTop(state, top);
            return 0;
        }

        lua51::pushNumber(state, entity);
        lua51::pushString(state, componentType);
        lua51::pushString(state, tag);
        if (lua51::pcall(state, 3, 1, 0) != 0 || lua51::type(state, -1) != lua51::typeNumber) {
            lua51::setTop(state, top);
            return 0;
        }

        const int component = static_cast<int>(lua51::toNumber(state, -1));
        lua51::setTop(state, top);
        return component;
    }

    void getAnimation(lua51::lua_State* state, int player, char* animation, std::size_t animationSize) {
        const int sprite = getTaggedComponent(state, player, "SpriteComponent", "character");
        if (sprite == 0) {
            strncpy_s(animation, animationSize, "stand", _TRUNCATE);
            return;
        }

        const int top = lua51::getTop(state);
        lua51::getGlobal(state, "ComponentGetValue2");
        if (lua51::type(state, -1) == lua51::typeFunction) {
            lua51::pushNumber(state, sprite);
            lua51::pushString(state, "rect_animation");
            if (lua51::pcall(state, 2, 1, 0) == 0 && lua51::type(state, -1) == lua51::typeString) {
                const char* value = lua51::toString(state, -1);
                if (value != nullptr && value[0] != '\0') {
                    strncpy_s(animation, animationSize, value, _TRUNCATE);
                    lua51::setTop(state, top);
                    return;
                }
            }
        }
        lua51::setTop(state, top);
        strncpy_s(animation, animationSize, "stand", _TRUNCATE);
    }

    int getActiveItem(lua51::lua_State* state, int player) {
        const int component = getComponent(state, player, "Inventory2Component");
        if (component == 0) {
            return 0;
        }

        const int top = lua51::getTop(state);
        lua51::getGlobal(state, "ComponentGetValue2");
        if (lua51::type(state, -1) != lua51::typeFunction) {
            lua51::setTop(state, top);
            return 0;
        }

        lua51::pushNumber(state, component);
        lua51::pushString(state, "mActiveItem");
        if (lua51::pcall(state, 2, 1, 0) != 0 || lua51::type(state, -1) != lua51::typeNumber) {
            lua51::setTop(state, top);
            return 0;
        }

        const int item = static_cast<int>(lua51::toNumber(state, -1));
        lua51::setTop(state, top);
        return item;
    }

    bool hasTag(lua51::lua_State* state, int entity, const char* tag) {
        const int top = lua51::getTop(state);
        lua51::getGlobal(state, "EntityHasTag");
        if (lua51::type(state, -1) != lua51::typeFunction) {
            lua51::setTop(state, top);
            return false;
        }

        lua51::pushNumber(state, entity);
        lua51::pushString(state, tag);
        if (lua51::pcall(state, 2, 1, 0) != 0 || lua51::type(state, -1) != lua51::typeBoolean) {
            lua51::setTop(state, top);
            return false;
        }

        const bool found = lua51::toBoolean(state, -1);
        lua51::setTop(state, top);
        return found;
    }

    bool getWand(lua51::lua_State* state, int player, int& wand, char* image, std::size_t imageSize, float& offsetX, float& offsetY,
        float& x, float& y, float& rotation, float& scaleX, float& scaleY) {
        wand = getActiveItem(state, player);
        if (wand == 0 || !hasTag(state, wand, "wand")) {
            return false;
        }

        const int sprite = getComponent(state, wand, "AbilityComponent");
        if (sprite == 0) {
            return false;
        }

        const int top = lua51::getTop(state);
        lua51::getGlobal(state, "ComponentGetValue2");
        if (lua51::type(state, -1) != lua51::typeFunction) {
            lua51::setTop(state, top);
            return false;
        }
        lua51::pushNumber(state, sprite);
        lua51::pushString(state, "sprite_file");
        if (lua51::pcall(state, 2, 1, 0) != 0 || lua51::type(state, -1) != lua51::typeString) {
            lua51::setTop(state, top);
            return false;
        }
        const char* const value = lua51::toString(state, -1);
        if (value == nullptr || value[0] == '\0') {
            lua51::setTop(state, top);
            return false;
        }
        strncpy_s(image, imageSize, value, _TRUNCATE);
        lua51::setTop(state, top);

        // Ability sprite definitions provide their own held-image pivot.
        // Item sprite offsets and the item's world pose are not this pose.
        offsetX = offsetY = 0;
        return true;
    }
}

void player_sync::update(lua51::lua_State* state) {
    if (state == nullptr || !lua51::ready()) {
        return;
    }

    int player = 0;
    if (!getPlayer(state, player)) {
        return;
    }

    const DWORD now = GetTickCount();
    const DWORD previous = static_cast<DWORD>(InterlockedCompareExchange(&p_lastUpdate, 0, 0));
    if (now - previous < p_updateTime) {
        return;
    }
    InterlockedExchange(&p_lastUpdate, static_cast<LONG>(now));

    if (InterlockedCompareExchange(&p_foundPlayer, TRUE, FALSE) == FALSE) {
        monitor::write("log", "Local player found");
    }

    float x = 0.0f;
    float y = 0.0f;
    if (!getTransform(state, player, x, y)) {
        return;
    }

    float velocityX = 0.0f;
    float velocityY = 0.0f;
    getVelocity(state, player, velocityX, velocityY);
    float aimX = 0.0f;
    float aimY = 0.0f;
    getAim(state, getControls(state, player), aimX, aimY);

    const char* facing = "right";
    if (aimX < 0.0f) {
        facing = "left";
    }

    network::PlayerState playerState{};
    playerState.x = x;
    playerState.y = y;
    playerState.velocityX = velocityX;
    playerState.velocityY = velocityY;
    playerState.aimX = aimX;
    playerState.aimY = aimY;
    playerState.facingLeft = aimX < 0.0f;
    playerState.onGround = getComponentBool(state, player, "CharacterDataComponent", "is_on_ground");
    playerState.flying = getComponentBool(state, player, "ControlsComponent", "mButtonDownFly");
    getAnimation(state, player, playerState.animation, sizeof(playerState.animation));
    playerState.armScaleX = 1.0f;
    playerState.armScaleY = 1.0f;
    float armWorldX = x;
    float armWorldY = y;
    const int arm = getChild(state, player, "player_arm_r");
    playerState.hasArm = arm != 0 && getFullTransform(state, arm, armWorldX, armWorldY,
        playerState.armRotation, playerState.armScaleX, playerState.armScaleY);
    if (playerState.hasArm) {
        playerState.armX = armWorldX - x;
        playerState.armY = armWorldY - y;
    }
    int wand = 0;
    float wandWorldX = 0.0f;
    float wandWorldY = 0.0f;
    playerState.wandScaleX = 1.0f;
    playerState.wandScaleY = 1.0f;
    playerState.hasWand = getWand(state, player, wand, playerState.wandSprite, sizeof(playerState.wandSprite),
        playerState.wandOffsetX, playerState.wandOffsetY, wandWorldX, wandWorldY, playerState.wandRotation,
        playerState.wandScaleX, playerState.wandScaleY);
    network::sendPlayer(playerState);

    char text[256]{};
    _snprintf_s(text, sizeof(text), _TRUNCATE, "%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%s", x, y, velocityX, velocityY, aimX, aimY, facing);
    monitor::write("player", text);
}

void player_sync::forget(lua51::lua_State*) {
}
