#include "lan_discovery.h"
#include "network.h"
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <algorithm>
#include <cstring>

namespace {
    constexpr unsigned short discoveryPort = 27889;
    constexpr char query[] = "WAND-LAN-33?";
    constexpr char reply[] = "WAND-LAN-33!";
    SOCKET socketHandle = INVALID_SOCKET;
    bool advertising = false;
    DWORD lastQuery = 0;
    std::vector<lan_discovery::Session> entries;
}

bool lan_discovery::open(bool host) {
    close();
    if (!network::init()) return false;
    socketHandle = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (socketHandle == INVALID_SOCKET) return false;
    BOOL enabled = TRUE;
    setsockopt(socketHandle, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char*>(&enabled), sizeof(enabled));
    if (host) setsockopt(socketHandle, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&enabled), sizeof(enabled));
    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_port = htons(host ? discoveryPort : 0);
    u_long nonblocking = 1;
    if (bind(socketHandle, reinterpret_cast<sockaddr*>(&local), sizeof(local)) != 0 || ioctlsocket(socketHandle, FIONBIO, &nonblocking) != 0) {
        close();
        return false;
    }
    advertising = host;
    lastQuery = GetTickCount() - 1000;
    return true;
}

void lan_discovery::close() {
    if (socketHandle != INVALID_SOCKET) closesocket(socketHandle);
    socketHandle = INVALID_SOCKET;
    advertising = false;
    entries.clear();
}

void lan_discovery::update() {
    if (socketHandle == INVALID_SOCKET) return;
    const DWORD now = GetTickCount();
    if (!advertising && now - lastQuery >= 1000) {
        lastQuery = now;
        sockaddr_in target{};
        target.sin_family = AF_INET;
        target.sin_port = htons(discoveryPort);
        target.sin_addr.s_addr = htonl(INADDR_BROADCAST);
        sendto(socketHandle, query, sizeof(query), 0, reinterpret_cast<sockaddr*>(&target), sizeof(target));
        target.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        sendto(socketHandle, query, sizeof(query), 0, reinterpret_cast<sockaddr*>(&target), sizeof(target));
    }
    for (int packet = 0; packet < 32; ++packet) {
        char buffer[128]{};
        sockaddr_in source{};
        int sourceSize = sizeof(source);
        const int size = recvfrom(socketHandle, buffer, sizeof(buffer), 0, reinterpret_cast<sockaddr*>(&source), &sourceSize);
        if (size == SOCKET_ERROR) break;
        if (advertising) {
            if (network::status() != network::Status::hosting || size != sizeof(query) || std::memcmp(buffer, query, sizeof(query)) != 0) continue;
            char response[80]{};
            std::memcpy(response, reply, sizeof(reply));
            DWORD length = 63;
            GetComputerNameA(response + sizeof(reply), &length);
            sendto(socketHandle, response, sizeof(response), 0, reinterpret_cast<sockaddr*>(&source), sourceSize);
        } else {
            if (size != 80 || std::memcmp(buffer, reply, sizeof(reply)) != 0 || source.sin_port != htons(discoveryPort)) continue;
            char address[INET_ADDRSTRLEN]{};
            if (!InetNtopA(AF_INET, &source.sin_addr, address, sizeof(address))) continue;
            std::string name;
            for (int i = sizeof(reply); i < 79 && buffer[i] && name.size() < 32; ++i) {
                const unsigned char c = buffer[i];
                name += c >= 32 && c <= 126 ? c : '?';
            }
            if (name.empty()) name = "Noita LAN game";
            auto found = std::find_if(entries.begin(), entries.end(), [&](const Session& entry) { return entry.address == address; });
            if (found != entries.end()) { found->seen = now; found->name = name; }
            else if (entries.size() < 64) entries.push_back({address, name, gamePort, now});
        }
    }
    entries.erase(std::remove_if(entries.begin(), entries.end(), [&](const Session& entry) { return now - entry.seen > 3500; }), entries.end());
}

const std::vector<lan_discovery::Session>& lan_discovery::sessions() { return entries; }
