#include "control.h"

#include "monitor.h"
#include "network.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <cstdlib>
#include <cstring>

namespace {
    constexpr int p_packetSize = 256;
    INIT_ONCE p_started = INIT_ONCE_STATIC_INIT;
    SOCKET p_socket = INVALID_SOCKET;
    unsigned short p_port = 0;
    bool p_ready = false;

    bool getPort(const char* text, unsigned short& port) {
        if (text == nullptr || text[0] == '\0') {
            return false;
        }

        char* end = nullptr;
        const unsigned long value = std::strtoul(text, &end, 10);
        if (end == text || end == nullptr || end[0] != '\0' || value == 0 || value > 65535) {
            return false;
        }
        port = static_cast<unsigned short>(value);
        return true;
    }

    void runCommand(char* packet) {
        char* context = nullptr;
        const char* magic = strtok_s(packet, "|", &context);
        const char* command = strtok_s(nullptr, "|", &context);
        if (magic == nullptr || command == nullptr || std::strcmp(magic, "WNDC1") != 0) {
            return;
        }

        if (std::strcmp(command, "host") == 0) {
            unsigned short port = 0;
            if (getPort(strtok_s(nullptr, "|", &context), port)) {
                monitor::write("log", "Monitor requested host");
                network::host(port);
            }
            return;
        }
        if (std::strcmp(command, "join") == 0) {
            const char* address = strtok_s(nullptr, "|", &context);
            unsigned short port = 0;
            if (address != nullptr && address[0] != '\0' && getPort(strtok_s(nullptr, "|", &context), port)) {
                monitor::write("log", "Monitor requested join");
                network::join(address, port);
            }
            return;
        }
        if (std::strcmp(command, "disconnect") == 0) {
            monitor::write("log", "Monitor requested disconnect");
            network::stop();
        }
    }

    DWORD WINAPI receiveCommands(LPVOID) {
        for (;;) {
            char packet[p_packetSize]{};
            sockaddr_in sender{};
            int senderSize = sizeof(sender);
            const int size = recvfrom(p_socket, packet, sizeof(packet) - 1, 0, reinterpret_cast<sockaddr*>(&sender), &senderSize);
            if (size <= 0) {
                continue;
            }
            if (sender.sin_family != AF_INET || ntohl(sender.sin_addr.s_addr) != INADDR_LOOPBACK) {
                continue;
            }
            packet[size] = '\0';
            runCommand(packet);
        }
    }

    BOOL CALLBACK startControl(PINIT_ONCE, PVOID, PVOID*) {
        p_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (p_socket == INVALID_SOCKET) {
            return TRUE;
        }

        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = 0;
        if (bind(p_socket, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == SOCKET_ERROR) {
            closesocket(p_socket);
            p_socket = INVALID_SOCKET;
            return TRUE;
        }

        int addressSize = sizeof(address);
        if (getsockname(p_socket, reinterpret_cast<sockaddr*>(&address), &addressSize) == SOCKET_ERROR) {
            closesocket(p_socket);
            p_socket = INVALID_SOCKET;
            return TRUE;
        }
        p_port = ntohs(address.sin_port);

        const HANDLE thread = CreateThread(nullptr, 0, receiveCommands, nullptr, 0, nullptr);
        if (thread == nullptr) {
            closesocket(p_socket);
            p_socket = INVALID_SOCKET;
            p_port = 0;
            return TRUE;
        }
        CloseHandle(thread);
        p_ready = true;
        return TRUE;
    }
}

bool control::init() {
    InitOnceExecuteOnce(&p_started, startControl, nullptr, nullptr);
    return p_ready;
}

unsigned short control::port() {
    return p_port;
}
