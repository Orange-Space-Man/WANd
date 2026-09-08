#pragma once

#include <cstdint>

namespace network {
    struct PlayerState {
        float x;
        float y;
        float velocityX;
        float velocityY;
        float aimX;
        float aimY;
        bool facingLeft;
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
    void sendPlayer(const PlayerState& state);
    bool getRemotePlayer(PlayerState& state);
}
