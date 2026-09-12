#pragma once

#include <windows.h>

#include <cstdint>

namespace noita {
    inline constexpr char pauseAnchor[] = "ending_no_game_over_menu";
    inline constexpr unsigned char gameGlobalPattern[] = { 0xE8, 0x00, 0x00, 0x00, 0x00, 0x8B, 0x40, 0x48, 0x8B, 0x00, 0xC1, 0xE8, 0x02, 0xA8, 0x01 };
    inline constexpr char gameGlobalMask[] = "x????xxxxxxxxxx";
    inline constexpr std::uintptr_t finalNewGameStartRva = 0x002CC030;
    inline constexpr std::uintptr_t mainMenuHeartbeatRva = 0x002E406C;
    inline constexpr std::uintptr_t worldSeedRva = 0x00E05004;
    inline constexpr std::uintptr_t worldSeedOverrideFlagRva = 0x00E06FAA;

    bool init();
    HMODULE game();
    HMODULE lua();
}
