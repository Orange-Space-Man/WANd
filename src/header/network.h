#pragma once

#include <cstdint>

namespace network {
    struct ProjectileEvent {
        std::uint32_t kind = 0, objectId = 0;
        char explosion[4096]{};
        char path[256];
        char flash[256];
        float values[15];
    };
    bool sendProjectile(const ProjectileEvent& event);
    bool takeProjectile(ProjectileEvent& event);
    struct PlayerState {
        float x;
        float y;
        float velocityX;
        float velocityY;
        float aimX;
        float aimY;
        bool facingLeft;
        bool onGround;
        bool flying;
        char animation[32];
        bool hasArm;
        float armX;
        float armY;
        float armRotation;
        float armScaleX;
        float armScaleY;
        bool hasWand;
        bool heldObject;
        char flaskMaterial[64];
        char wandSprite[256];
        float wandOffsetX;
        float wandOffsetY;
        float wandGripX;
        float wandGripY;
        float wandRotation;
        float wandScaleX;
        float wandScaleY;
        std::uint32_t outfit;
    };

    enum class Status : long {
        stopped,
        hosting,
        joining,
        connected
    };

    bool init();
    bool host(std::uint16_t port);
    bool join(const char* address, std::uint16_t port);
    void stop();
    Status status();
    const char* statusText();
    const char* role();
    bool isHost();
    bool beginRun(std::uint32_t seed);
    bool runReady();
    bool takeRun(std::uint32_t& seed);
    void sendPlayer(const PlayerState& state);
    bool getRemotePlayer(PlayerState& state, std::uint32_t& sequence);
}
