// Guest memory helpers shared by the native renderer, the game-side recorder
// and tools/replay.
#pragma once

#include <cstdint>

namespace replay {

// Guest virtual address -> physical (0xE0000000 views are offset by 4 KB).
inline uint32_t GuestToPhysical(uint32_t address) {
  return (address & 0x1FFFFFFF) + (address >= 0xE0000000u ? 0x1000u : 0u);
}

inline uint32_t LoadBE32(const uint8_t* p) {
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}

}  // namespace replay
