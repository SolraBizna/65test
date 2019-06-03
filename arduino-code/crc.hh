#ifndef CRCHH
#define CRCHH

#include <stdlib.h>
#include <inttypes.h>

class CRC {
  static const uint32_t CRC_TABLE[256];
  uint32_t crc = 0xFFFFFFFF;
public:
  inline void update(uint8_t b) {
    crc = CRC_TABLE[(crc ^ b) & 255] ^ (crc >> 8);
  }
  inline void update(const uint8_t* p, size_t rem) {
    while(rem-- > 0) {
      update(*p++);
    }
  }
  inline uint32_t result() const {
    return ~crc;
  }
};

#endif
