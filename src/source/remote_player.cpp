#include "remote_player.h"

#include "monitor.h"
#include "network.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>
#include <regex>
#include <filesystem>
#include "wand_image_grip.h"
#include "outfits.h"

namespace {
    constexpr DWORD p_updateTime = 16;
    constexpr DWORD p_snapshotTime = 50;
    constexpr DWORD p_maxExtrapolation = 100;
    int p_entity = 0;
    int p_sprite = 0;
    int p_arm = 0;
    int p_wandEntity = 0;
    int p_wandSpriteComponent = 0;
    int p_controls = 0;
    int p_inventory = 0;
    int p_quickInventory = 0;
    char p_wandSprite[256]{};
    float p_x = 0.0f;
    float p_y = 0.0f;
    float p_fromX = 0.0f;
    float p_fromY = 0.0f;
    float p_toX = 0.0f;
    float p_toY = 0.0f;
    float p_velocityX = 0.0f;
    float p_velocityY = 0.0f;
    float p_aimX = 1.0f;
    float p_aimY = 0.0f;
    float p_fromAimX = 1.0f;
    float p_fromAimY = 0.0f;
    float p_toAimX = 1.0f;
    float p_toAimY = 0.0f;
    char p_animation[32]{};
    std::uint32_t p_sequence = 0;
    DWORD p_snapshotStart = 0;
    DWORD p_snapshotDuration = p_snapshotTime;
    DWORD p_lastPacket = 0;
    DWORD p_lastUpdate = 0;
    bool p_loggedHandHotspot = false;
    bool p_tipValid = false;
    float p_tipX = 0, p_tipY = 0;

    bool beginCall(lua51::lua_State* state, const char* name, int top) {
        lua51::getGlobal(state, name);
        if (lua51::type(state, -1) == lua51::typeFunction) {
            return true;
        }
        lua51::setTop(state, top);
        return false;
    }

    void killEntity(lua51::lua_State* state, int entity) {
        if (entity == 0) {
            return;
        }

        const int top = lua51::getTop(state);
        if (beginCall(state, "EntityKill", top)) {
            lua51::pushNumber(state, entity);
            lua51::pcall(state, 1, 0, 0);
        }
        lua51::setTop(state, top);
    }

    void killRemote(lua51::lua_State* state) {
        outfits::forget(true);
        if (p_entity == 0 && p_arm == 0 && p_wandEntity == 0) {
            return;
        }

        killEntity(state, p_wandEntity);
        killEntity(state, p_arm);
        killEntity(state, p_entity);
        p_entity = 0;
        p_sprite = 0;
        p_arm = 0;
        p_wandEntity = 0;
        p_wandSpriteComponent = 0;
        p_controls = 0;
        p_inventory = 0;
        p_quickInventory = 0;
        p_wandSprite[0] = '\0';
        p_animation[0] = '\0';
        p_loggedHandHotspot = false;
        p_sequence = 0;
        monitor::write("log", "Remote player removed");
    }

    int getComponent(lua51::lua_State* state, int entity, const char* type, const char* tag = nullptr) {
        const int top = lua51::getTop(state);
        if (!beginCall(state, "EntityGetFirstComponentIncludingDisabled", top)) {
            return 0;
        }
        lua51::pushNumber(state, entity);
        lua51::pushString(state, type);
        int arguments = 2;
        if (tag != nullptr) {
            lua51::pushString(state, tag);
            arguments = 3;
        }
        if (lua51::pcall(state, arguments, 1, 0) != 0 || lua51::type(state, -1) != lua51::typeNumber) {
            lua51::setTop(state, top);
            return 0;
        }
        const int component = static_cast<int>(lua51::toNumber(state, -1));
        lua51::setTop(state, top);
        return component;
    }

    int getChild(lua51::lua_State* state, int entity, const char* tag) {
        const int top = lua51::getTop(state);
        if (!beginCall(state, "EntityGetAllChildren", top)) {
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
            if (beginCall(state, "EntityHasTag", top + 1)) {
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

    void removeTag(lua51::lua_State* state, int entity, const char* tag) {
        const int top = lua51::getTop(state);
        if (beginCall(state, "EntityRemoveTag", top)) {
            lua51::pushNumber(state, entity);
            lua51::pushString(state, tag);
            lua51::pcall(state, 2, 0, 0);
        }
        lua51::setTop(state, top);
    }

    void disableComponents(lua51::lua_State* state, int entity, const char* type) {
        const int top = lua51::getTop(state);
        if (!beginCall(state, "EntityGetComponentIncludingDisabled", top)) {
            return;
        }
        lua51::pushNumber(state, entity);
        lua51::pushString(state, type);
        if (lua51::pcall(state, 2, 1, 0) != 0 || lua51::type(state, -1) != lua51::typeTable) {
            lua51::setTop(state, top);
            return;
        }

        for (int index = 1; index <= 64; ++index) {
            lua51::rawGetIndex(state, -1, index);
            if (lua51::type(state, -1) != lua51::typeNumber) {
                lua51::setTop(state, top + 1);
                break;
            }
            const int component = static_cast<int>(lua51::toNumber(state, -1));
            lua51::setTop(state, top + 1);
            if (beginCall(state, "EntitySetComponentIsEnabled", top + 1)) {
                lua51::pushNumber(state, entity);
                lua51::pushNumber(state, component);
                lua51::pushBoolean(state, false);
                lua51::pcall(state, 3, 0, 0);
            }
            lua51::setTop(state, top + 1);
        }
        lua51::setTop(state, top);
    }

    void setNumber(lua51::lua_State* state, int component, const char* name, double value) {
        if (component == 0) {
            return;
        }
        const int top = lua51::getTop(state);
        if (beginCall(state, "ComponentSetValue2", top)) {
            lua51::pushNumber(state, component);
            lua51::pushString(state, name);
            lua51::pushNumber(state, value);
            lua51::pcall(state, 3, 0, 0);
        }
        lua51::setTop(state, top);
    }

    void setBoolean(lua51::lua_State* state, int component, const char* name, bool value) {
        if (component == 0) {
            return;
        }
        const int top = lua51::getTop(state);
        if (beginCall(state, "ComponentSetValue2", top)) {
            lua51::pushNumber(state, component);
            lua51::pushString(state, name);
            lua51::pushBoolean(state, value);
            lua51::pcall(state, 3, 0, 0);
        }
        lua51::setTop(state, top);
    }

    void setVector(lua51::lua_State* state, int component, const char* name, double x, double y) {
        if (component == 0) {
            return;
        }
        const int top = lua51::getTop(state);
        if (beginCall(state, "ComponentSetValue2", top)) {
            lua51::pushNumber(state, component);
            lua51::pushString(state, name);
            lua51::pushNumber(state, x);
            lua51::pushNumber(state, y);
            lua51::pcall(state, 4, 0, 0);
        }
        lua51::setTop(state, top);
    }

    void setComponentEnabled(lua51::lua_State* state, int entity, int component, bool enabled) {
        if (entity == 0 || component == 0) {
            return;
        }
        const int top = lua51::getTop(state);
        if (beginCall(state, "EntitySetComponentIsEnabled", top)) {
            lua51::pushNumber(state, entity);
            lua51::pushNumber(state, component);
            lua51::pushBoolean(state, enabled);
            lua51::pcall(state, 3, 0, 0);
        }
        lua51::setTop(state, top);
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

        if (beginCall(state, "EntityAddComponent2", top)) {
            lua51::pushNumber(state, entity);
            lua51::pushString(state, "PlatformShooterPlayerComponent");
            lua51::createTable(state, 0, 3);
            lua51::pushBoolean(state, false);
            lua51::setField(state, -2, "center_camera_on_this_entity");
            lua51::pushBoolean(state, false);
            lua51::setField(state, -2, "move_camera_with_aim");
            lua51::pushNumber(state, 60.0);
            lua51::setField(state, -2, "aiming_reticle_distance_from_character");
            lua51::pcall(state, 3, 0, 0);
        }
        lua51::setTop(state, top);

        if (beginCall(state, "EntityAddComponent2", top)) {
            lua51::pushNumber(state, entity);
            lua51::pushString(state, "CharacterDataComponent");
            lua51::createTable(state, 0, 1);
            lua51::pushNumber(state, 0.0);
            lua51::setField(state, -2, "gravity");
            lua51::pcall(state, 3, 0, 0);
        }
        lua51::setTop(state, top);

        if (beginCall(state, "EntityAddComponent2", top)) {
            lua51::pushNumber(state, entity);
            lua51::pushString(state, "CharacterPlatformingComponent");
            lua51::createTable(state, 0, 2);
            lua51::pushBoolean(state, true);
            lua51::setField(state, -2, "mouse_look");
            lua51::pushBoolean(state, true);
            lua51::setField(state, -2, "keyboard_look");
            lua51::pcall(state, 3, 0, 0);
        }
        lua51::setTop(state, top);

        if (beginCall(state, "EntityAddComponent2", top)) {
            lua51::pushNumber(state, entity);
            lua51::pushString(state, "ControlsComponent");
            lua51::createTable(state, 0, 1);
            lua51::pushBoolean(state, false);
            lua51::setField(state, -2, "enabled");
            if (lua51::pcall(state, 3, 1, 0) == 0 && lua51::type(state, -1) == lua51::typeNumber) {
                p_controls = static_cast<int>(lua51::toNumber(state, -1));
            }
        }
        lua51::setTop(state, top);

        if (beginCall(state, "EntityAddComponent2", top)) {
            lua51::pushNumber(state, entity);
            lua51::pushString(state, "Inventory2Component");
            lua51::createTable(state, 0, 2);
            lua51::pushNumber(state, 10.0);
            lua51::setField(state, -2, "quick_inventory_slots");
            lua51::pushNumber(state, 16.0);
            lua51::setField(state, -2, "full_inventory_slots_x");
            if (lua51::pcall(state, 3, 1, 0) == 0 && lua51::type(state, -1) == lua51::typeNumber) {
                p_inventory = static_cast<int>(lua51::toNumber(state, -1));
            }
        }
        lua51::setTop(state, top);

        if (beginCall(state, "EntityAddComponent2", top)) {
            lua51::pushNumber(state, entity);
            lua51::pushString(state, "GunComponent");
            lua51::createTable(state, 0, 0);
            lua51::pcall(state, 3, 0, 0);
        }
        lua51::setTop(state, top);

        if (beginCall(state, "EntityAddComponent2", top)) {
            lua51::pushNumber(state, entity);
            lua51::pushString(state, "SpriteComponent");
            lua51::createTable(state, 0, 6);
            lua51::pushString(state, "character");
            lua51::setField(state, -2, "_tags");
            lua51::pushString(state, "data/enemies_gfx/player.xml");
            lua51::setField(state, -2, "image_file");
            lua51::pushString(state, "stand");
            lua51::setField(state, -2, "rect_animation");
            lua51::pushNumber(state, 6.0);
            lua51::setField(state, -2, "offset_x");
            lua51::pushNumber(state, 14.0);
            lua51::setField(state, -2, "offset_y");
            lua51::pushNumber(state, 0.6);
            lua51::setField(state, -2, "z_index");
            if (lua51::pcall(state, 3, 1, 0) == 0 && lua51::type(state, -1) == lua51::typeNumber) {
                p_sprite = static_cast<int>(lua51::toNumber(state, -1));
            }
        }
        lua51::setTop(state, top);

        if (beginCall(state, "EntityAddComponent2", top)) {
            lua51::pushNumber(state, entity);
            lua51::pushString(state, "HotspotComponent");
            lua51::createTable(state, 0, 3);
            lua51::pushString(state, "hand");
            lua51::setField(state, -2, "_tags");
            lua51::pushString(state, "hand");
            lua51::setField(state, -2, "sprite_hotspot_name");
            lua51::pushBoolean(state, true);
            lua51::setField(state, -2, "transform_with_scale");
            lua51::pcall(state, 3, 0, 0);
        }
        lua51::setTop(state, top);

        if (beginCall(state, "EntityAddComponent2", top)) {
            lua51::pushNumber(state, entity);
            lua51::pushString(state, "HotspotComponent");
            lua51::createTable(state, 0, 3);
            lua51::pushString(state, "right_arm_root");
            lua51::setField(state, -2, "_tags");
            lua51::pushString(state, "right_arm_start");
            lua51::setField(state, -2, "sprite_hotspot_name");
            lua51::pushBoolean(state, true);
            lua51::setField(state, -2, "transform_with_scale");
            lua51::pcall(state, 3, 0, 0);
        }
        lua51::setTop(state, top);

        if (!beginCall(state, "EntityCreateNew", top)) {
            killEntity(state, entity);
            return 0;
        }
        lua51::pushString(state, "arm_r");
        if (lua51::pcall(state, 1, 1, 0) == 0 && lua51::type(state, -1) == lua51::typeNumber) {
            p_arm = static_cast<int>(lua51::toNumber(state, -1));
        }
        lua51::setTop(state, top);

        if (beginCall(state, "EntityAddTag", top)) {
            lua51::pushNumber(state, p_arm);
            lua51::pushString(state, "player_arm_r");
            lua51::pcall(state, 2, 0, 0);
        }
        lua51::setTop(state, top);

        if (beginCall(state, "EntityAddComponent2", top)) {
            lua51::pushNumber(state, p_arm);
            lua51::pushString(state, "SpriteComponent");
            lua51::createTable(state, 0, 4);
            lua51::pushString(state, "with_item");
            lua51::setField(state, -2, "_tags");
            lua51::pushString(state, "data/enemies_gfx/player_arm.xml");
            lua51::setField(state, -2, "image_file");
            lua51::pushString(state, "default");
            lua51::setField(state, -2, "rect_animation");
            lua51::pushNumber(state, 0.59);
            lua51::setField(state, -2, "z_index");
            lua51::pcall(state, 3, 0, 0);
        }
        lua51::setTop(state, top);

        if (beginCall(state, "EntityAddComponent2", top)) {
            lua51::pushNumber(state, p_arm);
            lua51::pushString(state, "InheritTransformComponent");
            lua51::createTable(state, 0, 2);
            lua51::pushString(state, "right_arm_root");
            lua51::setField(state, -2, "parent_hotspot_tag");
            lua51::pushBoolean(state, true);
            lua51::setField(state, -2, "only_position");
            lua51::pcall(state, 3, 0, 0);
        }
        lua51::setTop(state, top);

        if (beginCall(state, "EntityAddComponent2", top)) {
            lua51::pushNumber(state, p_arm);
            lua51::pushString(state, "HotspotComponent");
            lua51::createTable(state, 0, 3);
            lua51::pushString(state, "hand");
            lua51::setField(state, -2, "_tags");
            lua51::pushString(state, "hand");
            lua51::setField(state, -2, "sprite_hotspot_name");
            lua51::pushBoolean(state, true);
            lua51::setField(state, -2, "transform_with_scale");
            lua51::pcall(state, 3, 0, 0);
        }
        lua51::setTop(state, top);

        if (beginCall(state, "EntityAddChild", top)) {
            lua51::pushNumber(state, entity);
            lua51::pushNumber(state, p_arm);
            lua51::pcall(state, 2, 0, 0);
        }
        lua51::setTop(state, top);

        if (beginCall(state, "EntityCreateNew", top)) {
            lua51::pushString(state, "inventory_quick");
            if (lua51::pcall(state, 1, 1, 0) == 0 && lua51::type(state, -1) == lua51::typeNumber) {
                p_quickInventory = static_cast<int>(lua51::toNumber(state, -1));
            }
        }
        lua51::setTop(state, top);
        if (p_quickInventory != 0 && beginCall(state, "EntityAddChild", top)) {
            lua51::pushNumber(state, entity);
            lua51::pushNumber(state, p_quickInventory);
            lua51::pcall(state, 2, 0, 0);
        }
        lua51::setTop(state, top);

        if (p_sprite == 0 || p_controls == 0 || p_inventory == 0 || p_arm == 0 || p_quickInventory == 0) {
            monitor::write("log", "Remote player hierarchy incomplete");
            killEntity(state, entity);
            p_sprite = 0;
            p_controls = 0;
            p_inventory = 0;
            p_arm = 0;
            p_quickInventory = 0;
            return 0;
        }

        return entity;
    }

    int createDefinedEntity(lua51::lua_State* state, const network::PlayerState& player) {
        static const char* definition = R"xml(<Entity name="WANd remote player" tags="wand_remote_player">
  <StreamingKeepAliveComponent />
  <SpriteComponent _tags="character" image_file="data/enemies_gfx/player.xml" rect_animation="stand" offset_x="6" offset_y="14" z_index="0.6" />
  <HotspotComponent _tags="hand" sprite_hotspot_name="hand" transform_with_scale="1" />
  <HotspotComponent _tags="right_arm_root" sprite_hotspot_name="right_arm_start" transform_with_scale="1" />
  <HotspotComponent _tags="cape_root" sprite_hotspot_name="cape" />
  <Entity name="cape">
    <Base file="data/entities/verlet_chains/cape/cape.xml" />
  </Entity>
  <Entity name="arm_r" tags="player_arm_r">
    <SpriteComponent _tags="wand_remote_arm" image_file="data/enemies_gfx/player_arm.xml" rect_animation="default" z_index="0.59" />
    <InheritTransformComponent parent_hotspot_tag="right_arm_root" only_position="1" />
    <HotspotComponent _tags="hand" sprite_hotspot_name="hand" transform_with_scale="1" />
  </Entity>
  <Entity name="inventory_quick" tags="wand_remote_inventory" />
</Entity>)xml";
        const int top = lua51::getTop(state);
        if (!beginCall(state, "ModTextFileSetContent", top)) {
            return 0;
        }
        lua51::pushString(state, "mods/WANd/generated/remote_player.xml");
        lua51::pushString(state, definition);
        if (lua51::pcall(state, 2, 0, 0) != 0) {
            lua51::setTop(state, top);
            return 0;
        }
        lua51::setTop(state, top);

        if (!beginCall(state, "EntityLoad", top)) {
            return 0;
        }
        lua51::pushString(state, "mods/WANd/generated/remote_player.xml");
        lua51::pushNumber(state, player.x);
        lua51::pushNumber(state, player.y);
        if (lua51::pcall(state, 3, 1, 0) != 0 || lua51::type(state, -1) != lua51::typeNumber) {
            lua51::setTop(state, top);
            return 0;
        }
        const int entity = static_cast<int>(lua51::toNumber(state, -1));
        lua51::setTop(state, top);
        if (entity == 0) {
            return 0;
        }

        p_sprite = getComponent(state, entity, "SpriteComponent", "character");
        p_controls = getComponent(state, entity, "ControlsComponent");
        p_inventory = getComponent(state, entity, "Inventory2Component");
        p_arm = getChild(state, entity, "player_arm_r");
        p_quickInventory = getChild(state, entity, "wand_remote_inventory");
        if (p_sprite == 0 || p_arm == 0 || p_quickInventory == 0) {
            monitor::write("log", "Remote XML hierarchy incomplete");
            killEntity(state, entity);
            p_sprite = 0;
            p_controls = 0;
            p_inventory = 0;
            p_arm = 0;
            p_quickInventory = 0;
            return 0;
        }
        return entity;
    }

    void killWand(lua51::lua_State* state) {
        p_tipValid = false;
        if (p_wandEntity == 0) {
            return;
        }
        killEntity(state, p_wandEntity);
        p_wandEntity = 0;
        p_wandSpriteComponent = 0;
        p_wandSprite[0] = '\0';
        p_loggedHandHotspot = false;
    }

    bool getWorldHotspot(lua51::lua_State* state, int entity, const char* tag, float& x, float& y) {
        const int top = lua51::getTop(state);
        if (!beginCall(state, "EntityGetHotspot", top)) {
            return false;
        }
        lua51::pushNumber(state, entity);
        lua51::pushString(state, tag);
        lua51::pushBoolean(state, true);
        if (lua51::pcall(state, 3, 2, 0) != 0 || lua51::type(state, -2) != lua51::typeNumber || lua51::type(state, -1) != lua51::typeNumber) {
            lua51::setTop(state, top);
            return false;
        }
        x = static_cast<float>(lua51::toNumber(state, -2));
        y = static_cast<float>(lua51::toNumber(state, -1));
        lua51::setTop(state, top);
        return true;
    }

    std::string gripSprite(lua51::lua_State* state, const char* source, float handX, float handY) {
        const std::string path(source);
        const bool isXml = path.size() >= 4 && path.substr(path.size() - 4) == ".xml";
        const int top = lua51::getTop(state);
        std::string xml;
        if (isXml) {
        if (!beginCall(state, "ModTextFileGetContent", top)) return path;
        lua51::pushString(state, source);
        if (lua51::pcall(state, 1, 1, 0) != 0 || lua51::type(state, -1) != lua51::typeString) {
            lua51::setTop(state, top);
            return path;
        }
        xml = lua51::toString(state, -1);
        lua51::setTop(state, top);
        }
        auto attribute = [](const std::string& text, const char* name) {
            std::smatch match;
            return std::regex_search(text, match, std::regex(std::string("\\b") + name + "\\s*=\\s*[\"']([^\"']*)[\"']")) ? match[1].str() : std::string{};
        };
        std::smatch rootMatch;
        if (isXml && !std::regex_search(xml, rootMatch, std::regex("<Sprite\\b[^>]*>"))) return path;
        std::string png = isXml ? attribute(rootMatch.str(), "filename") : path;
        std::string frame;
        if (isXml) {
            const std::regex frames("<RectAnimation\\b[^>]*>");
            for (std::sregex_iterator it(xml.begin(), xml.end(), frames), end; it != end; ++it) {
                if (frame.empty()) frame = it->str();
                if (attribute(it->str(), "name") == "default") { frame = it->str(); break; }
            }
        }
        auto number = [&](const char* name) { const auto value = attribute(frame, name); return value.empty() ? 0 : std::atoi(value.c_str()); };
        int x = number("pos_x"), y = number("pos_y"), width = number("frame_width"), height = number("frame_height");
        wand_image::Grip grip{};
        if (!wand_image::decode(png, x, y, width, height, grip)) {
            monitor::write("error", "Wand image grip decoding failed");
            return {};
        }
        xml = "<Sprite filename=\"" + png + "\" default_animation=\"default\"><RectAnimation name=\"default\" pos_x=\"" + std::to_string(x)
            + "\" pos_y=\"" + std::to_string(y) + "\" frame_width=\"" + std::to_string(width) + "\" frame_height=\"" + std::to_string(height)
            + "\" frame_count=\"1\" frame_wait=\"1\" loop=\"1\" has_offset=\"1\" offset_x=\"" + std::to_string(grip.x - handX)
            + "\" offset_y=\"" + std::to_string(grip.y - handY) + "\" /></Sprite>";
        static std::uint64_t spriteGeneration = 0;
        const std::string generated = "mods/WANd/generated/grip_" + std::to_string(GetCurrentProcessId()) + "_" + std::to_string(GetTickCount64()) + "_" + std::to_string(++spriteGeneration) + ".xml";
        std::error_code error;
        std::filesystem::create_directories("mods/WANd/generated", error);
        if (error) { monitor::write("error", "Cannot create wand sprite directory"); return {}; }
        std::ofstream output(generated, std::ios::binary);
        output.write(xml.data(), xml.size());
        output.close();
        if (!output) { monitor::write("error", "Cannot write calculated wand sprite"); return {}; }
        monitor::write("log", generated.c_str());
        p_tipX = grip.tipX + 1.0f - grip.x + handX;
        p_tipY = grip.tipY - grip.y + handY;
        p_tipValid = true;
        return generated;
    }

    void createWand(lua51::lua_State* state, const network::PlayerState& player) {
        static float previousHandX = 0, previousHandY = 0;
        static float previousOffsetX = 0, previousOffsetY = 0;
        static bool previousHeldObject = false;
        static std::string previousFlaskMaterial;
        // This visual replica has no equipped inventory item. Its arm must
        // follow replicated visibility, not the native `with_item` toggle.
        const int armSprite = getComponent(state, p_arm, "SpriteComponent", "wand_remote_arm");
        setComponentEnabled(state, p_arm, armSprite, player.hasArm && player.hasWand);
        if (p_entity == 0 || !player.hasArm || !player.hasWand || player.wandSprite[0] == '\0') {
            killWand(state);
            return;
        }
        float handX = 0, handY = 0;
        const int hotspotTop = lua51::getTop(state);
        if (!beginCall(state, "EntityGetHotspot", hotspotTop)) return;
        lua51::pushNumber(state, p_arm);
        lua51::pushString(state, "hand");
        lua51::pushBoolean(state, false); // sprite-local coordinates, before flip/rotation
        lua51::pushBoolean(state, true);
        if (lua51::pcall(state, 4, 2, 0) != 0 || lua51::type(state, -2) != lua51::typeNumber || lua51::type(state, -1) != lua51::typeNumber) {
            lua51::setTop(state, hotspotTop);
            return;
        }
        handX = static_cast<float>(lua51::toNumber(state, -2));
        handY = static_cast<float>(lua51::toNumber(state, -1));
        lua51::setTop(state, hotspotTop);
        if (p_wandEntity != 0 && std::strcmp(p_wandSprite, player.wandSprite) == 0
            && previousHeldObject == player.heldObject
            && previousFlaskMaterial == player.flaskMaterial
            && previousHandX == handX && previousHandY == handY
            && previousOffsetX == player.wandOffsetX && previousOffsetY == player.wandOffsetY) {
            setComponentEnabled(state, p_wandEntity, p_wandSpriteComponent, true);
            return;
        }

        killWand(state);
        const int top = lua51::getTop(state);
        if (!beginCall(state, "EntityCreateNew", top)) {
            return;
        }
        lua51::pushString(state, "WANd remote hand visual");
        if (lua51::pcall(state, 1, 1, 0) != 0 || lua51::type(state, -1) != lua51::typeNumber) {
            lua51::setTop(state, top);
            return;
        }
        p_wandEntity = static_cast<int>(lua51::toNumber(state, -1));
        lua51::setTop(state, top);
        if (p_wandEntity == 0) {
            return;
        }

        const std::string imagePath = player.heldObject ? std::string(player.wandSprite) : gripSprite(state, player.wandSprite, handX, handY);
        if (imagePath.empty()) { killWand(state); return; }
        const bool rawImage = imagePath.size() < 4 || imagePath.substr(imagePath.size() - 4) != ".xml";
        if (!beginCall(state, "EntityAddComponent2", top)) {
            killWand(state);
            return;
        }
        lua51::pushNumber(state, p_wandEntity);
        lua51::pushString(state, "SpriteComponent");
        lua51::createTable(state, 0, 6);
        lua51::pushString(state, "wand_remote_sprite");
        lua51::setField(state, -2, "_tags");
        lua51::pushString(state, imagePath.c_str());
        lua51::setField(state, -2, "image_file");
        lua51::pushString(state, "default");
        lua51::setField(state, -2, "rect_animation");
        lua51::pushNumber(state, player.heldObject ? player.wandOffsetX : (rawImage ? player.wandOffsetX - handX : 0));
        lua51::setField(state, -2, "offset_x");
        lua51::pushNumber(state, player.heldObject ? player.wandOffsetY : (rawImage ? player.wandOffsetY - handY : 0));
        lua51::setField(state, -2, "offset_y");
        lua51::pushNumber(state, 0.595);
        lua51::setField(state, -2, "z_index");
        lua51::pushBoolean(state, true);
        lua51::setField(state, -2, "update_transform");
        if (lua51::pcall(state, 3, 1, 0) != 0 || lua51::type(state, -1) != lua51::typeNumber) {
            lua51::setTop(state, top);
            killWand(state);
            return;
        }
        p_wandSpriteComponent = static_cast<int>(lua51::toNumber(state, -1));
        lua51::setTop(state, top);

        if (player.heldObject && player.flaskMaterial[0]) {
            int material = 0;
            if (beginCall(state, "CellFactory_GetType", top)) {
                lua51::pushString(state, player.flaskMaterial);
                if (lua51::pcall(state, 1, 1, 0) == 0 && lua51::type(state, -1) == lua51::typeNumber)
                    material = static_cast<int>(lua51::toNumber(state, -1));
                lua51::setTop(state, top);
            }
            if (material > 0) {
                for (const char* type : {"MaterialInventoryComponent", "PotionComponent"}) {
                    if (!beginCall(state, "EntityAddComponent2", top)) { killWand(state); return; }
                    lua51::pushNumber(state, p_wandEntity);
                    lua51::pushString(state, type);
                    lua51::createTable(state, 0, 6);
                    if (std::strcmp(type, "MaterialInventoryComponent") == 0) {
                        for (const char* field : {"drop_as_item", "on_death_spill", "do_reactions_explosions", "do_reactions_entities"}) {
                            lua51::pushBoolean(state, false); lua51::setField(state, -2, field);
                        }
                        lua51::pushNumber(state, 0); lua51::setField(state, -2, "do_reactions");
                    } else {
                        lua51::pushNumber(state, material); lua51::setField(state, -2, "custom_color_material");
                        lua51::pushNumber(state, 0); lua51::setField(state, -2, "spray_velocity_coeff");
                        lua51::pushBoolean(state, false); lua51::setField(state, -2, "body_colored");
                    }
                    if (lua51::pcall(state, 3, 1, 0) != 0 || lua51::type(state, -1) != lua51::typeNumber) {
                        lua51::setTop(state, top); killWand(state); return;
                    }
                    lua51::setTop(state, top);
                }
                if (beginCall(state, "AddMaterialInventoryMaterial", top)) {
                    lua51::pushNumber(state, p_wandEntity);
                    lua51::pushString(state, player.flaskMaterial);
                    lua51::pushNumber(state, 1);
                    lua51::pcall(state, 3, 0, 0);
                    lua51::setTop(state, top);
                }
            }
        }
        if (!player.heldObject) {
        if (!beginCall(state, "EntityAddComponent2", top)) {
            killWand(state);
            return;
        }
        lua51::pushNumber(state, p_wandEntity);
        lua51::pushString(state, "InheritTransformComponent");
        lua51::createTable(state, 0, 2);
        lua51::pushString(state, "right_arm_root");
        lua51::setField(state, -2, "parent_hotspot_tag");
        // Held-image coordinates use the shoulder, just like the arm image.
        // Both siblings inherit body position and receive the same arm pose.
        lua51::pushBoolean(state, true);
        lua51::setField(state, -2, "only_position");
        if (lua51::pcall(state, 3, 1, 0) != 0 || lua51::type(state, -1) != lua51::typeNumber) {
            lua51::setTop(state, top);
            killWand(state);
            return;
        }
        lua51::setTop(state, top);

        }
        if (!beginCall(state, "EntityAddChild", top)) {
            killWand(state);
            return;
        }
        lua51::pushNumber(state, p_entity);
        lua51::pushNumber(state, p_wandEntity);
        if (lua51::pcall(state, 2, 0, 0) != 0) {
            lua51::setTop(state, top);
            killWand(state);
            return;
        }
        lua51::setTop(state, top);

        setComponentEnabled(state, p_wandEntity, p_wandSpriteComponent, true);

        strncpy_s(p_wandSprite, sizeof(p_wandSprite), player.wandSprite, _TRUNCATE);
        previousHandX = handX;
        previousHandY = handY;
        previousOffsetX = player.wandOffsetX;
        previousOffsetY = player.wandOffsetY;
        previousHeldObject = player.heldObject;
        previousFlaskMaterial = player.flaskMaterial;
        monitor::write("log", "Wand visual: ability sprite, shared shoulder and arm pose");
        p_loggedHandHotspot = true;
    }

    void setPosition(lua51::lua_State* state, int entity, float x, float y, bool facingLeft) {
        const int top = lua51::getTop(state);
        if (beginCall(state, "EntitySetTransform", top)) {
            lua51::pushNumber(state, entity);
            lua51::pushNumber(state, x);
            lua51::pushNumber(state, y);
            lua51::pushNumber(state, 0);
            lua51::pushNumber(state, facingLeft ? -1 : 1);
            lua51::pushNumber(state, 1);
            lua51::pcall(state, 6, 0, 0);
        }
        lua51::setTop(state, top);
    }

    void applyHeldPose(lua51::lua_State* state, const network::PlayerState& player) {
        if (!player.hasArm || !p_arm) return;
        float x = 0, y = 0;
        if (!getWorldHotspot(state, p_entity, "right_arm_root", x, y)) return;
        const int top = lua51::getTop(state);
        for (int entity : {p_arm, p_wandEntity}) {
            if (!entity || !beginCall(state, "EntitySetTransform", top)) continue;
            const bool object = entity == p_wandEntity && player.heldObject;
            lua51::pushNumber(state, entity);
            lua51::pushNumber(state, x + (object ? player.wandGripX : 0));
            lua51::pushNumber(state, y + (object ? player.wandGripY : 0));
            lua51::pushNumber(state, object ? player.wandRotation : player.armRotation);
            lua51::pushNumber(state, object ? player.wandScaleX : 1);
            lua51::pushNumber(state, object ? player.wandScaleY : player.armScaleY);
            lua51::pcall(state, 6, 0, 0);
            lua51::setTop(state, top);
        }
    }

    void logAttachment(lua51::lua_State* state) {
        static DWORD lastSample = 0;
        static unsigned samples = 0;
        if (!p_arm || !p_wandEntity || samples >= 300 || GetTickCount() - lastSample < 200) return;
        lastSample = GetTickCount();
        float handX = 0, handY = 0;
        if (!getWorldHotspot(state, p_arm, "hand", handX, handY)) return;
        float pose[2][5]{};
        const int entities[] = {p_arm, p_wandEntity};
        const int top = lua51::getTop(state);
        int wandParent = 0;
        bool armVisible = false;
        const int armSprite = getComponent(state, p_arm, "SpriteComponent", "wand_remote_arm");
        if (beginCall(state, "ComponentGetIsEnabled", top)) {
            lua51::pushNumber(state, armSprite);
            if (lua51::pcall(state, 1, 1, 0) == 0) armVisible = lua51::toBoolean(state, -1);
            lua51::setTop(state, top);
        }
        if (beginCall(state, "EntityGetParent", top)) {
            lua51::pushNumber(state, p_wandEntity);
            if (lua51::pcall(state, 1, 1, 0) == 0)
                wandParent = static_cast<int>(lua51::toNumber(state, -1));
            lua51::setTop(state, top);
        }
        for (int i = 0; i < 2; ++i) {
            if (!beginCall(state, "EntityGetTransform", top)) return;
            lua51::pushNumber(state, entities[i]);
            if (lua51::pcall(state, 1, 5, 0) != 0) {
                lua51::setTop(state, top);
                return;
            }
            for (int j = 0; j < 5; ++j) pose[i][j] = static_cast<float>(lua51::toNumber(state, -5 + j));
            lua51::setTop(state, top);
        }
        char folder[MAX_PATH]{};
        if (!GetTempPathA(MAX_PATH, folder)) return;
        char path[MAX_PATH]{};
        _snprintf_s(path, sizeof(path), _TRUNCATE, "%sWANd-attachment-%lu.csv", folder, GetCurrentProcessId());
        FILE* file = nullptr;
        if (fopen_s(&file, path, samples == 0 ? "w" : "a") != 0 || !file) return;
        if (samples == 0) std::fprintf(file, "tick,arm_id,wand_id,hand_x,hand_y,arm_x,arm_y,arm_rotation,arm_sx,arm_sy,wand_x,wand_y,wand_rotation,wand_sx,wand_sy,sprite,wand_parent,arm_visible\n");
        std::fprintf(file, "%lu,%d,%d,%.4f,%.4f", lastSample, p_arm, p_wandEntity, handX, handY);
        for (const auto& entityPose : pose) for (float value : entityPose) std::fprintf(file, ",%.4f", value);
        std::fprintf(file, ",%s,%d,%d\n", p_wandSprite, wandParent, armVisible ? 1 : 0);
        std::fclose(file);
        ++samples;
    }

    bool entityAlive(lua51::lua_State* state, int entity) {
        const int top = lua51::getTop(state);
        if (!beginCall(state, "EntityGetIsAlive", top)) {
            return true;
        }
        lua51::pushNumber(state, entity);
        if (lua51::pcall(state, 1, 1, 0) != 0 || lua51::type(state, -1) != lua51::typeBoolean) {
            lua51::setTop(state, top);
            return true;
        }
        const bool alive = lua51::toBoolean(state, -1);
        lua51::setTop(state, top);
        return alive;
    }

    void setAim(lua51::lua_State* state) {
        const float mouseX = p_x + p_aimX * 60.0f;
        const float mouseY = p_y + p_aimY * 60.0f;
        setVector(state, p_controls, "mAimingVector", p_aimX * 60.0f, p_aimY * 60.0f);
        setVector(state, p_controls, "mAimingVectorNormalized", p_aimX, p_aimY);
        setVector(state, p_controls, "mAimingVectorNonZeroLatest", p_aimX, p_aimY);
        setVector(state, p_controls, "mMousePositionRaw", mouseX, mouseY);
        setVector(state, p_controls, "mMousePositionRawPrev", mouseX, mouseY);
        setVector(state, p_controls, "mMouseDelta", 0.0, 0.0);
        setVector(state, p_controls, "mMousePosition", mouseX, mouseY);
    }

    void setAnimation(lua51::lua_State* state, const char* animation) {
        if (p_sprite == 0 || animation == nullptr || std::strcmp(p_animation, animation) == 0) {
            return;
        }

        const int top = lua51::getTop(state);
        if (beginCall(state, "ComponentSetValue2", top)) {
            lua51::pushNumber(state, p_sprite);
            lua51::pushString(state, "rect_animation");
            lua51::pushString(state, animation);
            lua51::pcall(state, 3, 0, 0);
        }
        lua51::setTop(state, top);
        strncpy_s(p_animation, sizeof(p_animation), animation, _TRUNCATE);
    }

    void resetMotion() {
        p_x = 0.0f;
        p_y = 0.0f;
        p_fromX = 0.0f;
        p_fromY = 0.0f;
        p_toX = 0.0f;
        p_toY = 0.0f;
        p_velocityX = 0.0f;
        p_velocityY = 0.0f;
        p_aimX = 1.0f;
        p_aimY = 0.0f;
        p_fromAimX = 1.0f;
        p_fromAimY = 0.0f;
        p_toAimX = 1.0f;
        p_toAimY = 0.0f;
        p_sequence = 0;
        p_snapshotStart = 0;
        p_snapshotDuration = p_snapshotTime;
        p_lastPacket = 0;
    }

    void updateMotion(const network::PlayerState& player, std::uint32_t sequence, DWORD now) {
        if (sequence == 0 || sequence == p_sequence) {
            const DWORD elapsed = now - p_snapshotStart;
            if (elapsed > p_snapshotDuration) {
                const DWORD extra = (std::min)(elapsed - p_snapshotDuration, p_maxExtrapolation);
                p_x = p_toX + p_velocityX * static_cast<float>(extra) * 0.06f;
                p_y = p_toY + p_velocityY * static_cast<float>(extra) * 0.06f;
            }
            return;
        }

        p_fromX = p_x;
        p_fromY = p_y;
        p_toX = player.x;
        p_toY = player.y;
        p_velocityX = player.velocityX;
        p_velocityY = player.velocityY;
        p_fromAimX = p_aimX;
        p_fromAimY = p_aimY;
        p_toAimX = player.aimX;
        p_toAimY = player.aimY;
        p_sequence = sequence;
        DWORD duration = p_snapshotTime;
        if (p_lastPacket != 0) {
            duration = (std::clamp)(now - p_lastPacket, 20UL, 100UL);
        }
        p_snapshotStart = now;
        p_snapshotDuration = duration;
        p_lastPacket = now;

        if (p_snapshotDuration == 0) {
            p_x = p_toX;
            p_y = p_toY;
        }
    }

    void applyMotion(DWORD now) {
        if (p_snapshotDuration == 0) {
            return;
        }

        const DWORD elapsed = now - p_snapshotStart;
        if (elapsed <= p_snapshotDuration) {
            const float amount = static_cast<float>(elapsed) / static_cast<float>(p_snapshotDuration);
            p_x = p_fromX + (p_toX - p_fromX) * amount;
            p_y = p_fromY + (p_toY - p_fromY) * amount;
            p_aimX = p_fromAimX + (p_toAimX - p_fromAimX) * amount;
            p_aimY = p_fromAimY + (p_toAimY - p_fromAimY) * amount;
            const float length = std::sqrt(p_aimX * p_aimX + p_aimY * p_aimY);
            if (length > 0.001f) {
                p_aimX /= length;
                p_aimY /= length;
            }
        } else {
            const DWORD extra = (std::min)(elapsed - p_snapshotDuration, p_maxExtrapolation);
            p_x = p_toX + p_velocityX * static_cast<float>(extra) * 0.06f;
            p_y = p_toY + p_velocityY * static_cast<float>(extra) * 0.06f;
            p_aimX = p_toAimX;
            p_aimY = p_toAimY;
        }
    }
}

void remote_player::update(lua51::lua_State* state) {
    if (state == nullptr || !lua51::ready()) {
        return;
    }

    network::PlayerState player{};
    std::uint32_t sequence = 0;
    if (network::status() != network::Status::connected || !network::getRemotePlayer(player, sequence)) {
        killRemote(state);
        resetMotion();
        return;
    }

    const DWORD now = GetTickCount();
    if (now - p_lastUpdate < p_updateTime) {
        return;
    }
    p_lastUpdate = now;

    if (p_entity != 0 && !entityAlive(state, p_entity)) {
        killEntity(state, p_wandEntity);
        killEntity(state, p_arm);
        p_entity = 0;
        p_sprite = 0;
        p_arm = 0;
        p_wandEntity = 0;
        p_wandSpriteComponent = 0;
        p_controls = 0;
        p_inventory = 0;
        p_quickInventory = 0;
        p_wandSprite[0] = '\0';
        p_loggedHandHotspot = false;
    }
    if (p_entity == 0) {
        outfits::forget(true);
        p_entity = createDefinedEntity(state, player);
        if (p_entity == 0 || p_sprite == 0) {
            killRemote(state);
            return;
        }
        p_x = player.x;
        p_y = player.y;
        p_fromX = player.x;
        p_fromY = player.y;
        p_toX = player.x;
        p_toY = player.y;
        p_velocityX = player.velocityX;
        p_velocityY = player.velocityY;
        p_aimX = player.aimX;
        p_aimY = player.aimY;
        p_fromAimX = player.aimX;
        p_fromAimY = player.aimY;
        p_toAimX = player.aimX;
        p_toAimY = player.aimY;
        p_sequence = sequence;
        p_snapshotStart = now;
        p_snapshotDuration = p_snapshotTime;
        p_lastPacket = now;
        p_animation[0] = '\0';
        monitor::write("log", "Remote player created");
    } else {
        updateMotion(player, sequence, now);
        applyMotion(now);
    }

    setPosition(state, p_entity, p_x, p_y, player.facingLeft);
    outfits::apply(state, p_entity, player.outfit, true);
    setAnimation(state, player.animation[0] != '\0' ? player.animation : "stand");
    outfits::animate(state, p_entity, player.animation[0] != '\0' ? player.animation : "stand");
    createWand(state, player);
    applyHeldPose(state, player);
    logAttachment(state);
}

void remote_player::forget(lua51::lua_State* state) {
    outfits::forget(true);
    p_tipValid = false;
    if (state != nullptr) {
        killRemote(state);
    } else {
        p_entity = 0;
        p_sprite = 0;
        p_arm = 0;
        p_wandEntity = 0;
        p_wandSpriteComponent = 0;
        p_controls = 0;
        p_inventory = 0;
        p_quickInventory = 0;
        p_wandSprite[0] = '\0';
        p_animation[0] = '\0';
        p_loggedHandHotspot = false;
    }
    resetMotion();
}

bool remote_player::wandTip(lua51::lua_State* state, float& x, float& y) {
    if (!p_tipValid || !p_wandEntity) return false;
    const int top = lua51::getTop(state);
    if (!beginCall(state, "EntityGetTransform", top)) return false;
    lua51::pushNumber(state, p_wandEntity);
    if (lua51::pcall(state, 1, 5, 0) != 0) { lua51::setTop(state, top); return false; }
    float pose[5];
    for (int i = 0; i < 5; ++i) {
        if (lua51::type(state, -5 + i) != lua51::typeNumber) { lua51::setTop(state, top); return false; }
        pose[i] = static_cast<float>(lua51::toNumber(state, -5 + i));
    }
    lua51::setTop(state, top);
    const float dx = p_tipX * pose[3], dy = p_tipY * pose[4];
    x = pose[0] + std::cos(pose[2]) * dx - std::sin(pose[2]) * dy;
    y = pose[1] + std::sin(pose[2]) * dx + std::cos(pose[2]) * dy;
    return true;
}
