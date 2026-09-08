#include "monitor.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <cstdio>

namespace {
    constexpr unsigned short p_port = 27889;
    constexpr int p_textSize = 768;
    constexpr int p_packetSize = 1024;

    INIT_ONCE p_started = INIT_ONCE_STATIC_INIT;
    SRWLOCK p_sendLock = SRWLOCK_INIT;
    SOCKET p_socket = INVALID_SOCKET;
    sockaddr_in p_address{};

    BOOL CALLBACK startMonitor(PINIT_ONCE, PVOID, PVOID*) {
        p_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (p_socket == INVALID_SOCKET) {
            return TRUE;
        }

        p_address.sin_family = AF_INET;
        p_address.sin_port = htons(p_port);
        InetPtonA(AF_INET, "127.0.0.1", &p_address.sin_addr);
        return TRUE;
    }
}

bool monitor::init() {
    InitOnceExecuteOnce(&p_started, startMonitor, nullptr, nullptr);
    return p_socket != INVALID_SOCKET;
}

void monitor::write(const char* type, const char* text) {
    if (!init() || type == nullptr || text == nullptr) {
        return;
    }

    char clean[p_textSize]{};
    int cleanLength = 0;
    for (int index = 0; text[index] != '\0' && cleanLength < p_textSize - 1; ++index) {
        char character = text[index];
        if (character == '|' || character == '\r' || character == '\n') {
            character = ' ';
        }
        clean[cleanLength] = character;
        ++cleanLength;
    }

    char packet[p_packetSize]{};
    const int size = _snprintf_s(packet, sizeof(packet), _TRUNCATE, "WND1|%lu|%s|%s", GetCurrentProcessId(), type, clean);
    if (size <= 0) {
        return;
    }

    AcquireSRWLockExclusive(&p_sendLock);
    sendto(p_socket, packet, size, 0, reinterpret_cast<const sockaddr*>(&p_address), sizeof(p_address));
    ReleaseSRWLockExclusive(&p_sendLock);
}
