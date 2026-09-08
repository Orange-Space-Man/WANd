#include "noita.h"

namespace {
    HMODULE p_game = nullptr;
    HMODULE p_lua = nullptr;
}

bool noita::init() {
    p_game = GetModuleHandleW(nullptr);
    if (p_game == nullptr) {
        return false;
    }

    for (int attempt = 0; attempt < 300; ++attempt) {
        p_lua = GetModuleHandleW(L"lua51.dll");
        if (p_lua != nullptr) {
            return true;
        }
        Sleep(50);
    }
    return false;
}

HMODULE noita::game() {
    return p_game;
}

HMODULE noita::lua() {
    return p_lua;
}
