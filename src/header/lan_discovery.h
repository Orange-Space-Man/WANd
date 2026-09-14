#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace lan_discovery {
    constexpr std::uint16_t gamePort = 27888;
    struct Session { std::string address; std::string name; std::uint16_t port; std::uint32_t seen; };
    bool open(bool host);
    void close();
    void update();
    const std::vector<Session>& sessions();
}
