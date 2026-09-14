#include "bomb_sync.h"
#include "monitor.h"
#include <windows.h>
#include <unordered_map>
#include <unordered_set>
#include <filesystem>
#include <fstream>
#include <string>
#include <cstring>

namespace {
    struct Stack { lua51::lua_State* s; int top; explicit Stack(lua51::lua_State* state) : s(state), top(lua51::getTop(state)) {} ~Stack() { lua51::setTop(s, top); } };
    std::unordered_map<int, network::ProjectileEvent> owned;
    std::unordered_map<unsigned, int> proxies;
    std::unordered_set<unsigned> finished;
    std::unordered_map<int, DWORD> missingSince;
    unsigned nextId = 0;
    DWORD lastUpdate = 0;
    const std::string directory = "mods/WANd/generated/";
    std::string deathFile;
    void kill(lua51::lua_State* s, int id) {
        if (!id) return;
        Stack stack(s); lua51::getGlobal(s, "EntityKill"); lua51::pushNumber(s, id); lua51::pcall(s, 1, 0, 0);
    }
    bool pose(lua51::lua_State* s, int id, network::ProjectileEvent& event) {
        Stack stack(s); lua51::getGlobal(s, "EntityGetIsAlive"); lua51::pushNumber(s, id);
        if (lua51::pcall(s, 1, 1, 0) != 0 || !lua51::toBoolean(s, -1)) return false;
        lua51::setTop(s, stack.top); lua51::getGlobal(s, "EntityGetTransform"); lua51::pushNumber(s, id);
        if (lua51::pcall(s, 1, 5, 0) != 0) return false;
        for (int i = 0; i < 5; ++i) {
            if (lua51::type(s, -5 + i) != lua51::typeNumber) return false;
            event.values[i] = static_cast<float>(lua51::toNumber(s, -5 + i));
        }
        return true;
    }
    void transform(lua51::lua_State* s, int id, const network::ProjectileEvent& event) {
        Stack stack(s); lua51::getGlobal(s, "EntitySetTransform"); lua51::pushNumber(s, id);
        for (int i = 0; i < 5; ++i) lua51::pushNumber(s, event.values[i]);
        lua51::pcall(s, 6, 0, 0);
        lua51::setTop(s, stack.top);
        lua51::getGlobal(s, "EntityGetFirstComponentIncludingDisabled"); lua51::pushNumber(s, id); lua51::pushString(s, "PhysicsBodyComponent");
        if (lua51::pcall(s, 2, 1, 0) != 0 || lua51::type(s, -1) != lua51::typeNumber) return;
        const int body = static_cast<int>(lua51::toNumber(s, -1)); lua51::setTop(s, stack.top);
        lua51::getGlobal(s, "GameVecToPhysicsVec"); lua51::pushNumber(s, event.values[0]); lua51::pushNumber(s, event.values[1]);
        if (lua51::pcall(s, 2, 2, 0) != 0 || lua51::type(s, -2) != lua51::typeNumber || lua51::type(s, -1) != lua51::typeNumber) return;
        const double x = lua51::toNumber(s, -2), y = lua51::toNumber(s, -1); lua51::setTop(s, stack.top);
        lua51::getGlobal(s, "PhysicsComponentSetTransform"); lua51::pushNumber(s, body);
        lua51::pushNumber(s, x); lua51::pushNumber(s, y); lua51::pushNumber(s, event.values[2]);
        lua51::pushNumber(s, 0); lua51::pushNumber(s, 0); lua51::pushNumber(s, 0); lua51::pcall(s, 7, 0, 0);
    }
    int load(lua51::lua_State* s, const std::string& path, const network::ProjectileEvent& event) {
        Stack stack(s); lua51::getGlobal(s, "EntityLoad"); lua51::pushString(s, path.c_str());
        lua51::pushNumber(s, event.values[0]); lua51::pushNumber(s, event.values[1]);
        return lua51::pcall(s, 3, 1, 0) == 0 && lua51::type(s, -1) == lua51::typeNumber ? static_cast<int>(lua51::toNumber(s, -1)) : 0;
    }
    bool write(const std::string& path, const std::string& content) {
        std::error_code error; std::filesystem::create_directories(directory, error); if (error) return false;
        std::ofstream out(path, std::ios::binary); out << content; out.close(); return bool(out);
    }
    int died(lua51::lua_State* s) {
        if (lua51::type(s, 1) != lua51::typeNumber) return 0;
        auto it = owned.find(static_cast<int>(lua51::toNumber(s, 1))); if (it == owned.end()) return 0;
        auto event = it->second;
        for (int i = 0; i < 3; ++i) {
            if (lua51::type(s, i + 2) != lua51::typeNumber) return 0;
            event.values[i] = static_cast<float>(lua51::toNumber(s, i + 2));
        }
        event.kind = 4;
        if (lua51::type(s, 5) == lua51::typeString) {
            const char* config = lua51::toString(s, 5);
            if (config && config[0] && strlen(config) < sizeof(event.explosion)) {
                strcpy_s(event.explosion, config); event.kind = 3;
            }
        }
        if (network::sendProjectile(event)) { missingSince.erase(it->first); owned.erase(it); }
        else monitor::write("error", "Bomb death event queue full");
        return 0;
    }
}
bool bomb_sync::track(lua51::lua_State* s, int entity, const network::ProjectileEvent& input) {
    if (strcmp(input.path, "data/entities/projectiles/bomb.xml") != 0) return false;
    auto event = input; event.objectId = ++nextId; event.kind = 1; event.values[14] = 0;
    pose(s, entity, event); owned[entity] = event;
    if (!network::sendProjectile(event)) monitor::write("error", "Bomb spawn event queue full");
    return true;
}
void bomb_sync::receive(lua51::lua_State* s, const network::ProjectileEvent& event) {
    if (finished.count(event.objectId)) return;
    if (event.kind == 3 || event.kind == 4) {
        auto it = proxies.find(event.objectId);
        if (it != proxies.end()) { transform(s, it->second, event); kill(s, it->second); proxies.erase(it); }
        finished.insert(event.objectId);
        return;
    }
    if (event.kind == 1 && !proxies.count(event.objectId)) {
        const auto path = directory + "bomb_owner_driven_v1.xml";
        if (!write(path, R"xml(<Entity><Base file="data/entities/projectiles/bomb.xml">
<PhysicsBodyComponent is_kinematic="1" gridworld_box2d="0" hax_fix_going_through_ground="0" auto_clean="0" on_death_leave_physics_body="0" kills_entity="0" />
<VelocityComponent affect_physics_bodies="0" gravity_y="0" />
<ProjectileComponent lifetime="-1" on_lifetime_out_explode="0" on_death_explode="1" collide_with_world="0" />
<DamageModelComponent hp="1000000" fire_damage_amount="0" materials_damage="0" />
</Base></Entity>)xml")) return;
        const int entity = load(s, path, event);
        if (entity) proxies[event.objectId] = entity;
    }
    auto it = proxies.find(event.objectId); if (it != proxies.end()) transform(s, it->second, event);
}
void bomb_sync::update(lua51::lua_State* s) {
    install(s);
    if (network::status() != network::Status::connected) { forget(s); return; }
    if (GetTickCount() - lastUpdate < 33) return; lastUpdate = GetTickCount();
    for (auto it = owned.begin(); it != owned.end();) {
        auto& event = it->second;
        const bool alive = pose(s, it->first, event); event.kind = alive ? 2 : 4;
        if (!alive) {
            const auto pending = missingSince.emplace(it->first, GetTickCount());
            if (GetTickCount() - pending.first->second < 33) { ++it; continue; }
        } else missingSince.erase(it->first);
        const bool sent = network::sendProjectile(event);
        if (!alive && sent) { missingSince.erase(it->first); it = owned.erase(it); } else ++it;
    }
}
void bomb_sync::install(lua51::lua_State* s) { lua51::pushFunction(s, died); lua51::setGlobal(s, "WANdBombDeath"); }
void bomb_sync::forget(lua51::lua_State* s) {
    for (const auto& item : proxies) {
        Stack stack(s);
        lua51::getGlobal(s, "EntityGetFirstComponentIncludingDisabled"); lua51::pushNumber(s, item.second); lua51::pushString(s, "ProjectileComponent");
        if (lua51::pcall(s, 2, 1, 0) == 0 && lua51::type(s, -1) == lua51::typeNumber) {
            const int component = static_cast<int>(lua51::toNumber(s, -1));
            lua51::getGlobal(s, "ComponentSetValue2"); lua51::pushNumber(s, component);
            lua51::pushString(s, "on_death_explode"); lua51::pushBoolean(s, false); lua51::pcall(s, 3, 0, 0);
        }
        kill(s, item.second);
    }
    proxies.clear(); owned.clear(); finished.clear(); missingSince.clear(); lastUpdate = 0;
}
