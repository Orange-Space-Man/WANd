#include "network.h"

#include "monitor.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <cstdio>
#include <cstring>

namespace {
    constexpr std::uint32_t p_magic = 0x57414E44;
    constexpr std::uint16_t p_version = 1;
    constexpr std::uint16_t p_hello = 1;
    constexpr std::uint16_t p_welcome = 2;
    constexpr std::uint16_t p_player = 3;

    struct Handshake {
        std::uint32_t magic;
        std::uint16_t version;
        std::uint16_t type;
    };

    struct JoinRequest {
        char address[64];
        std::uint16_t port;
    };

    struct PacketHeader {
        std::uint32_t magic;
        std::uint16_t version;
        std::uint16_t type;
        std::uint32_t size;
    };

    struct PlayerPacket {
        std::uint32_t sequence;
        std::uint32_t x;
        std::uint32_t y;
        std::uint32_t velocityX;
        std::uint32_t velocityY;
        std::uint32_t aimX;
        std::uint32_t aimY;
        std::uint32_t facingLeft;
    };

    INIT_ONCE p_started = INIT_ONCE_STATIC_INIT;
    SRWLOCK p_socketLock = SRWLOCK_INIT;
    SRWLOCK p_playerLock = SRWLOCK_INIT;
    LONG p_ready = FALSE;
    LONG p_status = static_cast<LONG>(network::Status::stopped);
    LONG p_role = 0;
    SOCKET p_listener = INVALID_SOCKET;
    SOCKET p_peer = INVALID_SOCKET;
    network::PlayerState p_localPlayer{};
    network::PlayerState p_remotePlayer{};
    std::uint32_t p_localSequence = 0;
    bool p_remotePlayerReady = false;

    const char* getStatusText(network::Status status) {
        if (status == network::Status::hosting) {
            return "hosting";
        }
        if (status == network::Status::joining) {
            return "joining";
        }
        if (status == network::Status::connected) {
            return "connected";
        }
        return "stopped";
    }

    void setStatus(network::Status status) {
        InterlockedExchange(&p_status, static_cast<LONG>(status));
        if (status == network::Status::stopped) {
            InterlockedExchange(&p_role, 0);
        }
        monitor::write("state", getStatusText(status));
    }

    bool changeStatus(network::Status current, network::Status next) {
        const LONG changed = InterlockedCompareExchange(&p_status, static_cast<LONG>(next), static_cast<LONG>(current));
        if (changed != static_cast<LONG>(current)) {
            return false;
        }
        if (next == network::Status::stopped) {
            InterlockedExchange(&p_role, 0);
        }
        monitor::write("state", getStatusText(next));
        return true;
    }

    void setTimeout(SOCKET socket) {
        const DWORD timeout = 5000;
        setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
        setsockopt(socket, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
    }

    void setConnectedTimeout(SOCKET socket) {
        const DWORD receiveTimeout = 0;
        const DWORD sendTimeout = 1000;
        setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&receiveTimeout), sizeof(receiveTimeout));
        setsockopt(socket, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&sendTimeout), sizeof(sendTimeout));
    }

    bool sendData(SOCKET socket, const void* data, int size) {
        const char* bytes = static_cast<const char*>(data);
        int sent = 0;
        while (sent < size) {
            const int result = send(socket, bytes + sent, size - sent, 0);
            if (result <= 0) {
                return false;
            }
            sent += result;
        }
        return true;
    }

    bool receiveData(SOCKET socket, void* data, int size) {
        char* bytes = static_cast<char*>(data);
        int received = 0;
        while (received < size) {
            const int result = recv(socket, bytes + received, size - received, 0);
            if (result <= 0) {
                return false;
            }
            received += result;
        }
        return true;
    }

    bool sendHandshake(SOCKET socket, std::uint16_t type) {
        Handshake handshake{};
        handshake.magic = htonl(p_magic);
        handshake.version = htons(p_version);
        handshake.type = htons(type);
        return sendData(socket, &handshake, sizeof(handshake));
    }

    bool receiveHandshake(SOCKET socket, std::uint16_t type) {
        Handshake handshake{};
        if (!receiveData(socket, &handshake, sizeof(handshake))) {
            return false;
        }
        return ntohl(handshake.magic) == p_magic && ntohs(handshake.version) == p_version && ntohs(handshake.type) == type;
    }

    std::uint32_t packFloat(float value) {
        std::uint32_t packed = 0;
        memcpy(&packed, &value, sizeof(value));
        return htonl(packed);
    }

    float unpackFloat(std::uint32_t value) {
        const std::uint32_t packed = ntohl(value);
        float result = 0.0f;
        memcpy(&result, &packed, sizeof(result));
        return result;
    }

    void storeListener(SOCKET socket) {
        AcquireSRWLockExclusive(&p_socketLock);
        p_listener = socket;
        ReleaseSRWLockExclusive(&p_socketLock);
    }

    void storePeer(SOCKET socket) {
        AcquireSRWLockExclusive(&p_socketLock);
        p_peer = socket;
        ReleaseSRWLockExclusive(&p_socketLock);
    }

    bool isCurrentPeer(SOCKET socket) {
        bool current = false;
        AcquireSRWLockShared(&p_socketLock);
        current = p_peer == socket;
        ReleaseSRWLockShared(&p_socketLock);
        return current;
    }

    void closeListener(SOCKET socket) {
        bool close = false;
        AcquireSRWLockExclusive(&p_socketLock);
        if (p_listener == socket) {
            p_listener = INVALID_SOCKET;
            close = true;
        }
        ReleaseSRWLockExclusive(&p_socketLock);
        if (close) {
            closesocket(socket);
        }
    }

    void closePeer(SOCKET socket) {
        bool close = false;
        AcquireSRWLockExclusive(&p_socketLock);
        if (p_peer == socket) {
            p_peer = INVALID_SOCKET;
            close = true;
        }
        ReleaseSRWLockExclusive(&p_socketLock);
        if (close) {
            shutdown(socket, SD_BOTH);
            closesocket(socket);
        }
    }

    bool sendPlayerPacket(SOCKET socket, std::uint32_t sequence, const network::PlayerState& state) {
        PacketHeader header{};
        header.magic = htonl(p_magic);
        header.version = htons(p_version);
        header.type = htons(p_player);
        header.size = htonl(sizeof(PlayerPacket));

        PlayerPacket packet{};
        packet.sequence = htonl(sequence);
        packet.x = packFloat(state.x);
        packet.y = packFloat(state.y);
        packet.velocityX = packFloat(state.velocityX);
        packet.velocityY = packFloat(state.velocityY);
        packet.aimX = packFloat(state.aimX);
        packet.aimY = packFloat(state.aimY);
        std::uint32_t facingLeft = 0;
        if (state.facingLeft) {
            facingLeft = 1;
        }
        packet.facingLeft = htonl(facingLeft);
        if (!sendData(socket, &header, sizeof(header))) {
            return false;
        }
        return sendData(socket, &packet, sizeof(packet));
    }

    void storeRemotePlayer(const PlayerPacket& packet) {
        network::PlayerState state{};
        state.x = unpackFloat(packet.x);
        state.y = unpackFloat(packet.y);
        state.velocityX = unpackFloat(packet.velocityX);
        state.velocityY = unpackFloat(packet.velocityY);
        state.aimX = unpackFloat(packet.aimX);
        state.aimY = unpackFloat(packet.aimY);
        state.facingLeft = ntohl(packet.facingLeft) != 0;

        AcquireSRWLockExclusive(&p_playerLock);
        p_remotePlayer = state;
        p_remotePlayerReady = true;
        ReleaseSRWLockExclusive(&p_playerLock);

        const char* facing = "right";
        if (state.facingLeft) {
            facing = "left";
        }
        char text[256]{};
        _snprintf_s(text, sizeof(text), _TRUNCATE, "%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%s", state.x, state.y, state.velocityX, state.velocityY, state.aimX, state.aimY, facing);
        monitor::write("remote_player", text);
    }

    DWORD WINAPI sendPlayerUpdates(LPVOID parameter) {
        const SOCKET socket = static_cast<SOCKET>(reinterpret_cast<UINT_PTR>(parameter));
        std::uint32_t sentSequence = 0;
        while (isCurrentPeer(socket) && network::status() == network::Status::connected) {
            network::PlayerState state{};
            std::uint32_t sequence = 0;
            AcquireSRWLockShared(&p_playerLock);
            state = p_localPlayer;
            sequence = p_localSequence;
            ReleaseSRWLockShared(&p_playerLock);

            if (sequence != 0 && sequence != sentSequence) {
                if (!sendPlayerPacket(socket, sequence, state)) {
                    break;
                }
                sentSequence = sequence;
            }
            Sleep(10);
        }

        closePeer(socket);
        changeStatus(network::Status::connected, network::Status::stopped);
        return 0;
    }

    DWORD WINAPI receivePlayerUpdates(LPVOID parameter) {
        const SOCKET socket = static_cast<SOCKET>(reinterpret_cast<UINT_PTR>(parameter));
        while (isCurrentPeer(socket) && network::status() == network::Status::connected) {
            PacketHeader header{};
            if (!receiveData(socket, &header, sizeof(header))) {
                break;
            }
            if (ntohl(header.magic) != p_magic || ntohs(header.version) != p_version || ntohs(header.type) != p_player || ntohl(header.size) != sizeof(PlayerPacket)) {
                monitor::write("log", "Invalid player packet received");
                break;
            }

            PlayerPacket packet{};
            if (!receiveData(socket, &packet, sizeof(packet))) {
                break;
            }
            storeRemotePlayer(packet);
        }

        closePeer(socket);
        changeStatus(network::Status::connected, network::Status::stopped);
        return 0;
    }

    bool startPlayerUpdates(SOCKET socket) {
        setConnectedTimeout(socket);
        const HANDLE receiveThread = CreateThread(nullptr, 0, receivePlayerUpdates, reinterpret_cast<LPVOID>(static_cast<UINT_PTR>(socket)), 0, nullptr);
        if (receiveThread == nullptr) {
            return false;
        }
        CloseHandle(receiveThread);

        const HANDLE sendThread = CreateThread(nullptr, 0, sendPlayerUpdates, reinterpret_cast<LPVOID>(static_cast<UINT_PTR>(socket)), 0, nullptr);
        if (sendThread == nullptr) {
            closePeer(socket);
            return false;
        }
        CloseHandle(sendThread);
        return true;
    }

    BOOL CALLBACK startWinsock(PINIT_ONCE, PVOID, PVOID*) {
        WSADATA data{};
        if (WSAStartup(MAKEWORD(2, 2), &data) == 0) {
            InterlockedExchange(&p_ready, TRUE);
        }
        return TRUE;
    }

    DWORD WINAPI waitForPlayer(LPVOID parameter) {
        const SOCKET listener = static_cast<SOCKET>(reinterpret_cast<UINT_PTR>(parameter));
        sockaddr_in address{};
        int addressSize = sizeof(address);
        const SOCKET peer = accept(listener, reinterpret_cast<sockaddr*>(&address), &addressSize);
        closeListener(listener);
        if (peer == INVALID_SOCKET) {
            changeStatus(network::Status::hosting, network::Status::stopped);
            return 0;
        }

        storePeer(peer);
        setTimeout(peer);
        if (!receiveHandshake(peer, p_hello) || !sendHandshake(peer, p_welcome)) {
            closePeer(peer);
            changeStatus(network::Status::hosting, network::Status::stopped);
            return 0;
        }
        if (!changeStatus(network::Status::hosting, network::Status::connected)) {
            closePeer(peer);
            return 0;
        }

        if (!startPlayerUpdates(peer)) {
            closePeer(peer);
            changeStatus(network::Status::connected, network::Status::stopped);
            return 0;
        }

        OutputDebugStringA("[WANd] Player connected.\n");
        monitor::write("log", "Player connected");
        return 0;
    }

    DWORD WINAPI connectToPlayer(LPVOID parameter) {
        JoinRequest* const request = static_cast<JoinRequest*>(parameter);
        const SOCKET peer = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (peer == INVALID_SOCKET) {
            HeapFree(GetProcessHeap(), 0, request);
            changeStatus(network::Status::joining, network::Status::stopped);
            return 0;
        }

        storePeer(peer);
        setTimeout(peer);
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(request->port);
        const int validAddress = InetPtonA(AF_INET, request->address, &address.sin_addr);
        HeapFree(GetProcessHeap(), 0, request);
        if (validAddress != 1 || connect(peer, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == SOCKET_ERROR) {
            closePeer(peer);
            changeStatus(network::Status::joining, network::Status::stopped);
            return 0;
        }
        if (!sendHandshake(peer, p_hello) || !receiveHandshake(peer, p_welcome)) {
            closePeer(peer);
            changeStatus(network::Status::joining, network::Status::stopped);
            return 0;
        }
        if (!changeStatus(network::Status::joining, network::Status::connected)) {
            closePeer(peer);
            return 0;
        }

        if (!startPlayerUpdates(peer)) {
            closePeer(peer);
            changeStatus(network::Status::connected, network::Status::stopped);
            return 0;
        }

        OutputDebugStringA("[WANd] Connected to host.\n");
        monitor::write("log", "Connected to host");
        return 0;
    }
}

bool network::init() {
    InitOnceExecuteOnce(&p_started, startWinsock, nullptr, nullptr);
    return InterlockedCompareExchange(&p_ready, FALSE, FALSE) == TRUE;
}

bool network::host(std::uint16_t port) {
    if (!init() || port == 0) {
        return false;
    }

    stop();
    const SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener == INVALID_SOCKET) {
        return false;
    }

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(port);
    if (bind(listener, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == SOCKET_ERROR || listen(listener, 1) == SOCKET_ERROR) {
        closesocket(listener);
        return false;
    }

    setStatus(Status::hosting);
    InterlockedExchange(&p_role, 1);
    monitor::write("role", "host");
    storeListener(listener);
    const HANDLE thread = CreateThread(nullptr, 0, waitForPlayer, reinterpret_cast<LPVOID>(static_cast<UINT_PTR>(listener)), 0, nullptr);
    if (thread == nullptr) {
        closeListener(listener);
        setStatus(Status::stopped);
        return false;
    }
    CloseHandle(thread);
    OutputDebugStringA("[WANd] Hosting game.\n");
    monitor::write("log", "Hosting game");
    return true;
}

bool network::join(const char* address, std::uint16_t port) {
    if (!init() || address == nullptr || address[0] == '\0' || port == 0) {
        return false;
    }
    const std::size_t addressLength = std::strlen(address);
    if (addressLength >= 64) {
        return false;
    }

    stop();
    JoinRequest* const request = static_cast<JoinRequest*>(HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(JoinRequest)));
    if (request == nullptr) {
        return false;
    }
    memcpy(request->address, address, addressLength + 1);
    request->port = port;

    setStatus(Status::joining);
    InterlockedExchange(&p_role, 2);
    monitor::write("role", "client");
    const HANDLE thread = CreateThread(nullptr, 0, connectToPlayer, request, 0, nullptr);
    if (thread == nullptr) {
        HeapFree(GetProcessHeap(), 0, request);
        setStatus(Status::stopped);
        return false;
    }
    CloseHandle(thread);
    OutputDebugStringA("[WANd] Joining game.\n");
    monitor::write("log", "Joining game");
    return true;
}

void network::stop() {
    setStatus(Status::stopped);

    SOCKET listener = INVALID_SOCKET;
    SOCKET peer = INVALID_SOCKET;
    AcquireSRWLockExclusive(&p_socketLock);
    listener = p_listener;
    peer = p_peer;
    p_listener = INVALID_SOCKET;
    p_peer = INVALID_SOCKET;
    ReleaseSRWLockExclusive(&p_socketLock);

    if (listener != INVALID_SOCKET) {
        closesocket(listener);
    }
    if (peer != INVALID_SOCKET) {
        shutdown(peer, SD_BOTH);
        closesocket(peer);
    }

    AcquireSRWLockExclusive(&p_playerLock);
    p_remotePlayer = PlayerState{};
    p_remotePlayerReady = false;
    ReleaseSRWLockExclusive(&p_playerLock);
    monitor::write("remote_player", "");
}

network::Status network::status() {
    return static_cast<Status>(InterlockedCompareExchange(&p_status, 0, 0));
}

const char* network::statusText() {
    return getStatusText(status());
}

const char* network::role() {
    const LONG role = InterlockedCompareExchange(&p_role, 0, 0);
    if (role == 1) {
        return "host";
    }
    if (role == 2) {
        return "client";
    }
    return "none";
}

bool network::isHost() {
    return InterlockedCompareExchange(&p_role, 0, 0) == 1;
}

void network::sendPlayer(const PlayerState& state) {
    AcquireSRWLockExclusive(&p_playerLock);
    p_localPlayer = state;
    ++p_localSequence;
    if (p_localSequence == 0) {
        p_localSequence = 1;
    }
    ReleaseSRWLockExclusive(&p_playerLock);
}

bool network::getRemotePlayer(PlayerState& state) {
    bool ready = false;
    AcquireSRWLockShared(&p_playerLock);
    ready = p_remotePlayerReady;
    if (ready) {
        state = p_remotePlayer;
    }
    ReleaseSRWLockShared(&p_playerLock);
    return ready;
}
