#include "projectile_sync.h"
#include "network.h"
#include "monitor.h"
#include "remote_player.h"
#include "bomb_sync.h"
#include <windows.h>
#include <unordered_set>
#include <vector>
#include <cstring>

namespace {
    std::unordered_set<int> seen;
    DWORD lastUpdate = 0;
    enum Value { X, Y, Rotation, ScaleX, ScaleY, VelocityX, VelocityY, Damage, Lifetime, SpeedMin, SpeedMax, Gravity, OwnerX, OwnerY, AtMuzzle };
    struct Stack { lua51::lua_State* state; int top; explicit Stack(lua51::lua_State* s) : state(s), top(lua51::getTop(s)) {} ~Stack() { lua51::setTop(state, top); } };
    std::vector<int> tagged(lua51::lua_State* s, const char* tag) {
        Stack stack(s); std::vector<int> result;
        lua51::getGlobal(s, "EntityGetWithTag"); lua51::pushString(s, tag);
        if (lua51::pcall(s, 1, 1, 0) != 0 || lua51::type(s, -1) != lua51::typeTable) return result;
        for (int i = 1; i <= 16384; ++i) {
            lua51::rawGetIndex(s, -1, i);
            if (lua51::type(s, -1) != lua51::typeNumber) break;
            result.push_back(static_cast<int>(lua51::toNumber(s, -1)));
            lua51::setTop(s, stack.top + 1);
        }
        return result;
    }
    int component(lua51::lua_State* s, int entity, const char* type) {
        Stack stack(s); lua51::getGlobal(s, "EntityGetFirstComponentIncludingDisabled");
        lua51::pushNumber(s, entity); lua51::pushString(s, type);
        return lua51::pcall(s, 2, 1, 0) == 0 && lua51::type(s, -1) == lua51::typeNumber ? static_cast<int>(lua51::toNumber(s, -1)) : 0;
    }
    bool numbers(lua51::lua_State* s, int comp, const char* field, float* values, int count) {
        Stack stack(s); lua51::getGlobal(s, "ComponentGetValue2"); lua51::pushNumber(s, comp); lua51::pushString(s, field);
        if (lua51::pcall(s, 2, count, 0) != 0) return false;
        for (int i = 0; i < count; ++i) {
            if (lua51::type(s, -count + i) != lua51::typeNumber) return false;
            values[i] = static_cast<float>(lua51::toNumber(s, -count + i));
        }
        return true;
    }
    void set(lua51::lua_State* s, int comp, const char* field, const float* values, int count) {
        Stack stack(s); lua51::getGlobal(s, "ComponentSetValue2"); lua51::pushNumber(s, comp); lua51::pushString(s, field);
        for (int i = 0; i < count; ++i) lua51::pushNumber(s, values[i]);
        lua51::pcall(s, 2 + count, 0, 0);
    }
    bool capture(lua51::lua_State* s, int entity, int owner, network::ProjectileEvent& event) {
        Stack stack(s);
        const int projectile = component(s, entity, "ProjectileComponent"), velocity = component(s, entity, "VelocityComponent");
        float shooter = 0;
        if (!projectile || !velocity || !numbers(s, projectile, "mWhoShot", &shooter, 1) || shooter != owner) return false;
        lua51::getGlobal(s, "EntityGetFilename"); lua51::pushNumber(s, entity);
        if (lua51::pcall(s, 1, 1, 0) != 0 || lua51::type(s, -1) != lua51::typeString) return false;
        const char* path = lua51::toString(s, -1);
        if (!path || !path[0] || strlen(path) >= sizeof(event.path)) return false;
        strcpy_s(event.path, path); lua51::setTop(s, stack.top);
        lua51::getGlobal(s, "EntityGetTransform"); lua51::pushNumber(s, entity);
        if (lua51::pcall(s, 1, 5, 0) != 0) return false;
        for (int i = 0; i < 5; ++i) {
            if (lua51::type(s, -5 + i) != lua51::typeNumber) return false;
            event.values[i] = static_cast<float>(lua51::toNumber(s, -5 + i));
        }
        if (!numbers(s, velocity, "mVelocity", event.values + VelocityX, 2)) return false;
        const char* fields[] = {"damage", "lifetime", "speed_min", "speed_max"};
        for (int i = 0; i < 4; ++i) if (!numbers(s, projectile, fields[i], event.values + Damage + i, 1)) return false;
        if (!numbers(s, velocity, "gravity_y", event.values + Gravity, 1)) return false;
        lua51::setTop(s, stack.top);
        lua51::getGlobal(s, "ComponentGetValue2"); lua51::pushNumber(s, projectile); lua51::pushString(s, "muzzle_flash_file");
        if (lua51::pcall(s, 2, 1, 0) == 0 && lua51::type(s, -1) == lua51::typeString) {
            const char* flash = lua51::toString(s, -1);
            if (flash && strlen(flash) < sizeof(event.flash)) strcpy_s(event.flash, flash);
        }
        lua51::setTop(s, stack.top);
        float parent = 0, active = 0;
        const int inventory = component(s, owner, "Inventory2Component");
        if (numbers(s, projectile, "mEntityThatShot", &parent, 1)
            && inventory && numbers(s, inventory, "mActiveItem", &active, 1) && active > 0
            && (parent == owner || parent == active || parent == 0)) {
            lua51::getGlobal(s, "EntityGetTransform"); lua51::pushNumber(s, owner);
            if (lua51::pcall(s, 1, 2, 0) == 0 && lua51::type(s, -2) == lua51::typeNumber && lua51::type(s, -1) == lua51::typeNumber) {
                event.values[OwnerX] = static_cast<float>(lua51::toNumber(s, -2));
                event.values[OwnerY] = static_cast<float>(lua51::toNumber(s, -1));
                lua51::setTop(s, stack.top);
                lua51::getGlobal(s, "EntityGetHotspot"); lua51::pushNumber(s, active);
                lua51::pushString(s, "shoot_pos"); lua51::pushBoolean(s, true); lua51::pushBoolean(s, true);
                if (lua51::pcall(s, 4, 2, 0) == 0 && lua51::type(s, -2) == lua51::typeNumber && lua51::type(s, -1) == lua51::typeNumber) {
                    event.values[X] = static_cast<float>(lua51::toNumber(s, -2));
                    event.values[Y] = static_cast<float>(lua51::toNumber(s, -1));
                    event.values[AtMuzzle] = 1;
                    float initialLifetime = 0;
                    if (numbers(s, projectile, "mStartingLifetime", &initialLifetime, 1) && initialLifetime > 0)
                        event.values[Lifetime] = initialLifetime;
                }
            }
        }
        return true;
    }
    void spawn(lua51::lua_State* s, const network::ProjectileEvent& event) {
        Stack stack(s);
        auto owners = tagged(s, "wand_remote_player");
        if (owners.empty()) return;
        float v[15]; memcpy(v, event.values, sizeof(v));
        if (v[AtMuzzle] == 1) {
            lua51::getGlobal(s, "EntityGetTransform"); lua51::pushNumber(s, owners.front());
            if (lua51::pcall(s, 1, 2, 0) == 0 && lua51::type(s, -2) == lua51::typeNumber && lua51::type(s, -1) == lua51::typeNumber) {
                v[X] += static_cast<float>(lua51::toNumber(s, -2)) - v[OwnerX];
                v[Y] += static_cast<float>(lua51::toNumber(s, -1)) - v[OwnerY];
            }
            lua51::setTop(s, stack.top);
        }
        if (v[AtMuzzle] == 1) remote_player::wandTip(s, v[X], v[Y]);
        lua51::getGlobal(s, "EntityLoad"); lua51::pushString(s, event.path); lua51::pushNumber(s, v[X]); lua51::pushNumber(s, v[Y]);
        if (lua51::pcall(s, 3, 1, 0) != 0 || lua51::type(s, -1) != lua51::typeNumber) return;
        const int entity = static_cast<int>(lua51::toNumber(s, -1)); lua51::setTop(s, stack.top);
        if (!entity) return;
        const int projectile = component(s, entity, "ProjectileComponent"), velocity = component(s, entity, "VelocityComponent");
        if (!projectile || !velocity) {
            lua51::getGlobal(s, "EntityKill"); lua51::pushNumber(s, entity); lua51::pcall(s, 1, 0, 0); return;
        }
        const char* fields[] = {"damage", "lifetime", "speed_min", "speed_max"};
        for (int i = 0; i < 4; ++i) set(s, projectile, fields[i], v + Damage + i, 1);
        lua51::getGlobal(s, "ComponentSetValue2"); lua51::pushNumber(s, projectile);
        lua51::pushString(s, "muzzle_flash_file"); lua51::pushString(s, ""); lua51::pcall(s, 3, 0, 0);
        lua51::getGlobal(s, "GameShootProjectile"); lua51::pushNumber(s, owners.front());
        lua51::pushNumber(s, v[X]); lua51::pushNumber(s, v[Y]);
        lua51::pushNumber(s, v[X] + v[VelocityX]); lua51::pushNumber(s, v[Y] + v[VelocityY]);
        lua51::pushNumber(s, entity); lua51::pushBoolean(s, false);
        if (lua51::pcall(s, 7, 0, 0) != 0) {
            lua51::getGlobal(s, "EntityKill"); lua51::pushNumber(s, entity); lua51::pcall(s, 1, 0, 0); return;
        }
        set(s, velocity, "mVelocity", v + VelocityX, 2);
        set(s, velocity, "gravity_y", v + Gravity, 1);
        lua51::getGlobal(s, "EntitySetTransform"); lua51::pushNumber(s, entity);
        for (int i = 0; i < 5; ++i) lua51::pushNumber(s, v[i]);
        lua51::pcall(s, 6, 0, 0);
        seen.insert(entity);
        if (event.flash[0]) {
            lua51::getGlobal(s, "EntityLoad"); lua51::pushString(s, event.flash);
            lua51::pushNumber(s, v[X]); lua51::pushNumber(s, v[Y]);
            if (lua51::pcall(s, 3, 1, 0) == 0 && lua51::type(s, -1) == lua51::typeNumber) {
                const int flash = static_cast<int>(lua51::toNumber(s, -1));
                if (flash) {
                    const int sprite = component(s, flash, "SpriteComponent");
                    const float edge = 0;
                    if (sprite) set(s, sprite, "offset_x", &edge, 1);
                    lua51::getGlobal(s, "EntitySetTransform"); lua51::pushNumber(s, flash);
                    lua51::pushNumber(s, v[X]); lua51::pushNumber(s, v[Y]); lua51::pushNumber(s, v[Rotation]);
                    lua51::pcall(s, 4, 0, 0);
                }
            }
        }
    }
}
void projectile_sync::update(lua51::lua_State* state) {
    bomb_sync::update(state);
    if (network::status() != network::Status::connected) { forget(); return; }
    const DWORD now = GetTickCount(); if (now - lastUpdate < 8) return; lastUpdate = now;
    network::ProjectileEvent event{};
    for (int i = 0; i < 128 && network::takeProjectile(event); ++i) {
        if (event.kind) bomb_sync::receive(state, event); else spawn(state, event);
    }
    auto players = tagged(state, "player_unit"); if (players.empty()) return;
    auto entities = tagged(state, "projectile");
    auto playerProjectiles = tagged(state, "projectile_player");
    entities.insert(entities.end(), playerProjectiles.begin(), playerProjectiles.end());
    std::unordered_set<int> current(entities.begin(), entities.end());
    for (int entity : current) {
        if (seen.count(entity)) continue;
        event = {};
        if (capture(state, entity, players.front(), event) && !bomb_sync::track(state, entity, event) && !network::sendProjectile(event))
            monitor::write("error", "Outgoing projectile queue full");
    }
    seen.swap(current);
}
void projectile_sync::forget() { seen.clear(); lastUpdate = 0; }
