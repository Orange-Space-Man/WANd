#include "network.h"

#include "monitor.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <cstdio>
#include <cstring>
#include <deque>
#include <cmath>

namespace {
    constexpr std::uint32_t p_magic = 0x57414E44;
    constexpr std::uint16_t p_version = 33;
    constexpr std::uint16_t p_projectile = 6;
    struct ProjectilePacket { std::uint32_t kind, objectId; char explosion[4096]; char path[256]; char flash[256]; std::uint32_t values[15]; };
    SRWLOCK p_projectileLock = SRWLOCK_INIT;
    std::deque<network::ProjectileEvent> p_incomingProjectiles, p_outgoingProjectiles;
    constexpr std::uint16_t p_hello = 1;
    constexpr std::uint16_t p_welcome = 2;
    constexpr std::uint16_t p_player = 3;
    constexpr std::uint16_t p_runStart = 4;
    constexpr std::uint16_t p_runReady = 5;

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
        std::uint32_t onGround;
        std::uint32_t flying;
        char animation[32];
        std::uint32_t hasArm;
        std::uint32_t armX;
        std::uint32_t armY;
        std::uint32_t armRotation;
        std::uint32_t armScaleX;
        std::uint32_t armScaleY;
        std::uint32_t hasWand;
        char wandSprite[256];
        char flaskMaterial[64];
        std::uint32_t wandOffsetX;
        std::uint32_t wandOffsetY;
        std::uint32_t wandGripX;
        std::uint32_t wandGripY;
        std::uint32_t wandRotation;
        std::uint32_t wandScaleX;
        std::uint32_t wandScaleY;
        std::uint32_t outfit;
    };

    struct RunPacket {
        std::uint32_t run;
        std::uint32_t seed;
    };

    struct ReadyPacket {
        std::uint32_t run;
    };

    INIT_ONCE p_started = INIT_ONCE_STATIC_INIT;
    SRWLOCK p_socketLock = SRWLOCK_INIT;
    SRWLOCK p_playerLock = SRWLOCK_INIT;
    SRWLOCK p_sendLock = SRWLOCK_INIT;
    SRWLOCK p_runLock = SRWLOCK_INIT;
    LONG p_ready = FALSE;
    LONG p_status = static_cast<LONG>(network::Status::stopped);
    LONG p_role = 0;
    SOCKET p_listener = INVALID_SOCKET;
    SOCKET p_peer = INVALID_SOCKET;
    network::PlayerState p_localPlayer{};
    network::PlayerState p_remotePlayer{};
    std::uint32_t p_localSequence = 0;
    std::uint32_t p_remoteSequence = 0;
    bool p_remotePlayerReady = false;
    std::uint32_t p_nextRun = 0;
    std::uint32_t p_waitingRun = 0;
    std::uint32_t p_receivedSeed = 0;
    bool p_runIsReady = false;
    bool p_runReceived = false;

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
        if (status == network::Status::stopped) {
            AcquireSRWLockExclusive(&p_projectileLock);
            p_incomingProjectiles.clear(); p_outgoingProjectiles.clear();
            ReleaseSRWLockExclusive(&p_projectileLock);
        }
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
        std::uint32_t onGround = 0;
        if (state.onGround) {
            onGround = 1;
        }
        packet.onGround = htonl(onGround);
        std::uint32_t flying = 0;
        if (state.flying) {
            flying = 1;
        }
        packet.flying = htonl(flying);
        memcpy(packet.animation, state.animation, sizeof(packet.animation));
        packet.animation[sizeof(packet.animation) - 1] = '\0';
        packet.hasArm = htonl(state.hasArm ? 1U : 0U);
        packet.armX = packFloat(state.armX);
        packet.armY = packFloat(state.armY);
        packet.armRotation = packFloat(state.armRotation);
        packet.armScaleX = packFloat(state.armScaleX);
        packet.armScaleY = packFloat(state.armScaleY);
        std::uint32_t hasWand = 0;
        if (state.hasWand) {
            hasWand = 1;
        }
        packet.hasWand = htonl(hasWand);
        if (state.heldObject) packet.hasWand = htonl(hasWand | 2U);
        memcpy(packet.wandSprite, state.wandSprite, sizeof(packet.wandSprite));
        packet.wandSprite[sizeof(packet.wandSprite) - 1] = '\0';
        memcpy(packet.flaskMaterial, state.flaskMaterial, sizeof(packet.flaskMaterial));
        packet.flaskMaterial[sizeof(packet.flaskMaterial) - 1] = '\0';
        packet.wandOffsetX = packFloat(state.wandOffsetX);
        packet.wandOffsetY = packFloat(state.wandOffsetY);
        packet.wandGripX = packFloat(state.wandGripX);
        packet.wandGripY = packFloat(state.wandGripY);
        packet.wandRotation = packFloat(state.wandRotation);
        packet.wandScaleX = packFloat(state.wandScaleX);
        packet.wandScaleY = packFloat(state.wandScaleY);
        packet.outfit = htonl(state.outfit);
        bool sent = false;
        AcquireSRWLockExclusive(&p_sendLock);
        if (sendData(socket, &header, sizeof(header))) {
            sent = sendData(socket, &packet, sizeof(packet));
        }
        ReleaseSRWLockExclusive(&p_sendLock);
        return sent;
    }

    bool sendRunPacket(SOCKET socket, std::uint16_t type, const void* packet, std::uint32_t size) {
        PacketHeader header{};
        header.magic = htonl(p_magic);
        header.version = htons(p_version);
        header.type = htons(type);
        header.size = htonl(size);

        bool sent = false;
        AcquireSRWLockExclusive(&p_sendLock);
        if (sendData(socket, &header, sizeof(header))) {
            sent = sendData(socket, packet, static_cast<int>(size));
        }
        ReleaseSRWLockExclusive(&p_sendLock);
        return sent;
    }

    void storeRunStart(SOCKET socket, const RunPacket& packet) {
        const std::uint32_t run = ntohl(packet.run);
        const std::uint32_t seed = ntohl(packet.seed);
        if (run == 0 || seed == 0) {
            return;
        }

        AcquireSRWLockExclusive(&p_runLock);
        p_receivedSeed = seed;
        p_runReceived = true;
        ReleaseSRWLockExclusive(&p_runLock);

        ReadyPacket ready{};
        ready.run = htonl(run);
        sendRunPacket(socket, p_runReady, &ready, sizeof(ready));

        char text[64]{};
        _snprintf_s(text, sizeof(text), _TRUNCATE, "Run received, seed %lu", seed);
        monitor::write("log", text);
    }

    void storeRunReady(const ReadyPacket& packet) {
        const std::uint32_t run = ntohl(packet.run);
        bool ready = false;
        AcquireSRWLockExclusive(&p_runLock);
        if (run != 0 && run == p_waitingRun) {
            p_runIsReady = true;
            ready = true;
        }
        ReleaseSRWLockExclusive(&p_runLock);
        if (ready) {
            monitor::write("log", "Player ready for new run");
        }
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
        state.onGround = ntohl(packet.onGround) != 0;
        state.flying = ntohl(packet.flying) != 0;
        memcpy(state.animation, packet.animation, sizeof(state.animation));
        state.animation[sizeof(state.animation) - 1] = '\0';
        state.hasArm = ntohl(packet.hasArm) != 0;
        state.armX = unpackFloat(packet.armX);
        state.armY = unpackFloat(packet.armY);
        state.armRotation = unpackFloat(packet.armRotation);
        state.armScaleX = unpackFloat(packet.armScaleX);
        state.armScaleY = unpackFloat(packet.armScaleY);
        state.hasWand = (ntohl(packet.hasWand) & 1U) != 0;
        state.heldObject = (ntohl(packet.hasWand) & 2U) != 0;
        memcpy(state.wandSprite, packet.wandSprite, sizeof(state.wandSprite));
        state.wandSprite[sizeof(state.wandSprite) - 1] = '\0';
        memcpy(state.flaskMaterial, packet.flaskMaterial, sizeof(state.flaskMaterial));
        state.flaskMaterial[sizeof(state.flaskMaterial) - 1] = '\0';
        state.wandOffsetX = unpackFloat(packet.wandOffsetX);
        state.wandOffsetY = unpackFloat(packet.wandOffsetY);
        state.wandGripX = unpackFloat(packet.wandGripX);
        state.wandGripY = unpackFloat(packet.wandGripY);
        state.wandRotation = unpackFloat(packet.wandRotation);
        state.wandScaleX = unpackFloat(packet.wandScaleX);
        state.wandScaleY = unpackFloat(packet.wandScaleY);
        state.outfit = ntohl(packet.outfit);

        AcquireSRWLockExclusive(&p_playerLock);
        p_remotePlayer = state;
        p_remoteSequence = ntohl(packet.sequence);
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
            std::deque<network::ProjectileEvent> shots;
            AcquireSRWLockExclusive(&p_projectileLock);
            shots.swap(p_outgoingProjectiles);
            ReleaseSRWLockExclusive(&p_projectileLock);
            bool shotFailed = false;
            for (const auto& shot : shots) {
                ProjectilePacket packet{};
                packet.kind = htonl(shot.kind); packet.objectId = htonl(shot.objectId);
                memcpy(packet.explosion, shot.explosion, sizeof(packet.explosion));
                memcpy(packet.path, shot.path, sizeof(packet.path));
                memcpy(packet.flash, shot.flash, sizeof(packet.flash));
                for (int i = 0; i < 15; ++i) packet.values[i] = packFloat(shot.values[i]);
                if (!sendRunPacket(socket, p_projectile, &packet, sizeof(packet))) { shotFailed = true; break; }
            }
            if (shotFailed) break;
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
            if (ntohl(header.magic) != p_magic || ntohs(header.version) != p_version) {
                monitor::write("log", "Invalid network packet received");
                break;
            }

            const std::uint16_t type = ntohs(header.type);
            const std::uint32_t size = ntohl(header.size);
            if (type == p_projectile && size == sizeof(ProjectilePacket)) {
                ProjectilePacket packet{};
                if (!receiveData(socket, &packet, sizeof(packet))) break;
                network::ProjectileEvent event{};
                event.kind = ntohl(packet.kind); event.objectId = ntohl(packet.objectId);
                memcpy(event.explosion, packet.explosion, sizeof(event.explosion)); event.explosion[4095] = '\0';
                memcpy(event.path, packet.path, sizeof(event.path));
                event.path[255] = '\0';
                memcpy(event.flash, packet.flash, sizeof(event.flash));
                event.flash[255] = '\0';
                if (event.flash[0] && (strstr(event.flash, "..") != nullptr
                    || (strncmp(event.flash, "data/", 5) != 0 && strncmp(event.flash, "mods/", 5) != 0))) event.flash[0] = '\0';
                bool valid = event.kind <= 4 && (event.kind == 0 || (event.objectId != 0 && strcmp(event.path, "data/entities/projectiles/bomb.xml") == 0))
                    && event.path[0] != '\0' && strstr(event.path, "..") == nullptr
                    && (strncmp(event.path, "data/", 5) == 0 || strncmp(event.path, "mods/", 5) == 0);
                for (int i = 0; i < 15; ++i) {
                    event.values[i] = unpackFloat(packet.values[i]);
                    valid = valid && std::isfinite(event.values[i]);
                }
                if (valid) {
                    AcquireSRWLockExclusive(&p_projectileLock);
                    if (p_incomingProjectiles.size() < 1024) p_incomingProjectiles.push_back(event);
                    else monitor::write("error", "Incoming projectile queue full");
                    ReleaseSRWLockExclusive(&p_projectileLock);
                }
                continue;
            }
            if (type == p_player && size == sizeof(PlayerPacket)) {
                PlayerPacket packet{};
                if (!receiveData(socket, &packet, sizeof(packet))) {
                    break;
                }
                storeRemotePlayer(packet);
                continue;
            }
            if (type == p_runStart && size == sizeof(RunPacket)) {
                RunPacket packet{};
                if (!receiveData(socket, &packet, sizeof(packet))) {
                    break;
                }
                storeRunStart(socket, packet);
                continue;
            }
            if (type == p_runReady && size == sizeof(ReadyPacket)) {
                ReadyPacket packet{};
                if (!receiveData(socket, &packet, sizeof(packet))) {
                    break;
                }
                storeRunReady(packet);
                continue;
            }

            monitor::write("log", "Invalid network packet received");
            if (size > 4096) {
                break;
            }
            char ignored[4096]{};
            if (!receiveData(socket, ignored, static_cast<int>(size))) {
                break;
            }
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
    p_remoteSequence = 0;
    p_remotePlayerReady = false;
    ReleaseSRWLockExclusive(&p_playerLock);

    AcquireSRWLockExclusive(&p_runLock);
    p_waitingRun = 0;
    p_receivedSeed = 0;
    p_runIsReady = false;
    p_runReceived = false;
    ReleaseSRWLockExclusive(&p_runLock);
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

bool network::beginRun(std::uint32_t seed) {
    if (seed == 0 || status() != Status::connected || !isHost()) {
        return false;
    }

    SOCKET peer = INVALID_SOCKET;
    AcquireSRWLockShared(&p_socketLock);
    peer = p_peer;
    ReleaseSRWLockShared(&p_socketLock);
    if (peer == INVALID_SOCKET) {
        return false;
    }

    std::uint32_t run = 0;
    AcquireSRWLockExclusive(&p_runLock);
    ++p_nextRun;
    if (p_nextRun == 0) {
        p_nextRun = 1;
    }
    run = p_nextRun;
    p_waitingRun = run;
    p_runIsReady = false;
    ReleaseSRWLockExclusive(&p_runLock);

    RunPacket packet{};
    packet.run = htonl(run);
    packet.seed = htonl(seed);
    if (!sendRunPacket(peer, p_runStart, &packet, sizeof(packet))) {
        return false;
    }

    char text[64]{};
    _snprintf_s(text, sizeof(text), _TRUNCATE, "Starting shared run, seed %lu", seed);
    monitor::write("log", text);
    return true;
}

bool network::runReady() {
    bool ready = false;
    AcquireSRWLockShared(&p_runLock);
    ready = p_runIsReady;
    ReleaseSRWLockShared(&p_runLock);
    return ready;
}

bool network::takeRun(std::uint32_t& seed) {
    bool received = false;
    AcquireSRWLockExclusive(&p_runLock);
    if (p_runReceived) {
        seed = p_receivedSeed;
        p_runReceived = false;
        received = true;
    }
    ReleaseSRWLockExclusive(&p_runLock);
    return received;
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

bool network::getRemotePlayer(PlayerState& state, std::uint32_t& sequence) {
    bool ready = false;
    AcquireSRWLockShared(&p_playerLock);
    ready = p_remotePlayerReady;
    if (ready) {
        state = p_remotePlayer;
        sequence = p_remoteSequence;
    }
    ReleaseSRWLockShared(&p_playerLock);
    return ready;
}

bool network::sendProjectile(const ProjectileEvent& event) {
    if (status() != Status::connected) return false;
    AcquireSRWLockExclusive(&p_projectileLock);
    const bool accepted = p_outgoingProjectiles.size() < 1024;
    if (accepted) p_outgoingProjectiles.push_back(event);
    ReleaseSRWLockExclusive(&p_projectileLock);
    return accepted;
}

bool network::takeProjectile(ProjectileEvent& event) {
    AcquireSRWLockExclusive(&p_projectileLock);
    const bool available = !p_incomingProjectiles.empty();
    if (available) { event = p_incomingProjectiles.front(); p_incomingProjectiles.pop_front(); }
    ReleaseSRWLockExclusive(&p_projectileLock);
    return available;
}
