#pragma once

#include <stdint.h>

namespace vidili {

constexpr uint16_t packUniverse(uint8_t artNet, uint8_t artSubnet, uint8_t artUni) {
  return (uint16_t(artNet) << 8) | (uint16_t(artSubnet) << 4) | (artUni & 0x0F);
}

constexpr uint8_t applyMaster(uint8_t value, uint8_t master) {
  return master >= 255 ? value : uint8_t((uint16_t(value) * master) >> 8);
}

}  // namespace vidili