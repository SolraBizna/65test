#ifndef PACKETIOHH
#define PACKETIOHH

#include <array>

// Will be called if a protocol error occurs, such as:
// - Bad CRC
// - Bad framing
// - Wrong role
// - Dropped byte detected
// Doesn't allow for much of recovery. The application this was written for
// resets at the slightest error anyway.
extern void shutdown() __attribute__((noreturn));

namespace PacketIO {
  static const size_t MAX_PHYSICAL_PACKET_SIZE = 120;
  // the larger this is, the more RAM we will use
  static const size_t MAX_LOGICAL_PACKET_SIZE = MAX_PHYSICAL_PACKET_SIZE * 10;
  enum class Role {
    Sender, Receiver
  };
  uint8_t* getBuf();
  // Call only from Sender role
  // returns false if the packet was handled and the role remained the same,
  // true if the packet was handled and the role flipped.
  bool sendFromBuf(uint8_t packet_type, size_t length);
  // Call only from Receiver role
  // your handler should return:
  // - false if the packet was handled and the role should stay the same
  // - true if the packet was handled and the role should reverse
  // or shutdown under any other circumstance.
  // This returns the value returned by the handler, in addition to flipping
  // the role.
  bool recv(bool(*handler)(uint8_t, size_t, const uint8_t*));
  // Call only from SENDER.
  // Call this periodically if you run a long loop. Try to get it to run at
  // least once per 2 seconds.
  void pumpHeart();
};

#endif
