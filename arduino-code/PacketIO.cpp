#include "PacketIO.hh"

#include "Arduino.h"
#include "crc.hh"

// NOTE: THE SUBSET OF COBS THAT WE IMPLEMENT CANNOT RELIABLY HANDLE PACKETS
// LONGER THAN 254 BYTES! Our maximum physical packet size is 127 (counting
// baggage), so this doesn't bother us much.

using namespace PacketIO;

namespace {
  const int SOFT_TIMEOUT = 30000;
  const int HARD_TIMEOUT = 60000;
  const size_t HEADER_SPACE = 2; // type, length
  const size_t FOOTER_SPACE = 4; // CRC
  uint8_t buf[HEADER_SPACE + MAX_LOGICAL_PACKET_SIZE + FOOTER_SPACE];
  Role role = Role::Receiver;
  unsigned long lastReceiveTime = millis();
  void awaitAvailableSilent() {
    auto when = millis() + HARD_TIMEOUT;
    while(!Serial.available() && millis() < when)
      pmc_enable_sleepmode(0);
    if(!Serial.available()) {
      shutdown();
    }
    lastReceiveTime = millis();
  }
  void awaitAvailableWithEcho(int min) {
    auto soft = millis() + SOFT_TIMEOUT;
    while(Serial.available() < min && millis() < soft)
      pmc_enable_sleepmode(0);
    if(Serial.available() >= min) {
      lastReceiveTime = millis();
      return;
    }
    sendFromBuf(0xFF, 0);
    auto hard = soft + (HARD_TIMEOUT - SOFT_TIMEOUT);
    while(Serial.available() < min && millis() < hard)
      pmc_enable_sleepmode(0);
    if(Serial.available() >= min) {
      lastReceiveTime = millis();
      return;
    }
    shutdown();
  }
  class COBSInput {
    // -2 = EOF
    // -1 = need to read another byte
    // 0 = need to output a zero byte
    // anything else = this many non-zero bytes remaining
    int16_t status = -1;
    CRC crc;
  public:
    inline int get() {
      while(true) {
        switch(status) {
        case 0: {
          status = -1;
          crc.update(0);
          return 0;
        }
        default: {
          --status;
          if(!Serial.available())
            awaitAvailableSilent();
          auto tsugi = Serial.read();
          if(tsugi > 0) {
            crc.update(tsugi);
            return tsugi;
          }
          else shutdown();
          // not reached
        }
        case -1: {
          if(!Serial.available())
            awaitAvailableSilent();
          auto tsugi = Serial.read();
          if(tsugi == -1) shutdown();
          else if(tsugi == 0) {
            status = -2;
            return EOF;
          }
          else {
            status = tsugi - 1;
            continue; // go through the loop again
          }
        }
        case -2: return EOF;
        }
      }
    }
    uint8_t require() {
      auto result = get();
      if(result < 0) shutdown();
      return result;
    }
    inline uint32_t final_crc() {
      return crc.result();
    }
  };
  bool rawsend(const uint8_t* ptr, size_t rem) {
    auto packetType = *ptr;
    bool packetIsKeepalive = packetType == 0 && ptr[1] == 0;
    bool packetIsFragment = packetType == 0 && ptr[1] != 0;
    pumpHeart();
    while(rem > 0) {
      auto start = ptr;
      uint8_t runlength = 1;
      while(*ptr != 0 && rem > 0) {
        ++runlength;
        ++ptr;
        --rem;
      }
      Serial.write(runlength);
      if(start != ptr) {
        Serial.write(start, ptr - start);
      }
      if(rem > 0) {
        // *ptr == 0
        ++ptr;
        --rem;
        if(rem == 0) {
          Serial.write(uint8_t(1));
        }
      }
    }
    Serial.write(uint8_t(0));
    if(packetIsKeepalive) return false;
    while(true) {
      awaitAvailableWithEcho(3);
      if(Serial.read() != 0) shutdown();
      if(Serial.read() != 0) shutdown();
      switch(Serial.read()) {
      case 1:
        if(packetIsFragment) shutdown();
        return false;
      case 2:
        if(!packetIsFragment) shutdown();
        return false;
      case 3:
        if(packetIsFragment) shutdown();
        role = Role::Receiver;
        return true;
      case 7:
        // Heartbeat
        // ... which we do not care to acknowledge at this time, since WE are
        // waiting for YOU, kind sir!
        break;
      case 8:
        if(packetType != 0xFF) shutdown();
        return false;
      default:
        // something we did not expect...
        shutdown();
      }
    }
  }
}

uint8_t* PacketIO::getBuf() { return buf + HEADER_SPACE; }

bool PacketIO::sendFromBuf(uint8_t packet_type, size_t length) {
  if(role != Role::Sender) shutdown();
  if(length > MAX_LOGICAL_PACKET_SIZE) shutdown();
  uint8_t* ptr = buf + HEADER_SPACE;
  size_t rem = length;
  while(rem > MAX_PHYSICAL_PACKET_SIZE) {
    // Send a fragment.
    uint8_t saved[4];
    ptr[-2] = 0;
    ptr[-1] = MAX_PHYSICAL_PACKET_SIZE;
    saved[0] = ptr[MAX_PHYSICAL_PACKET_SIZE];
    saved[1] = ptr[MAX_PHYSICAL_PACKET_SIZE+1];
    saved[2] = ptr[MAX_PHYSICAL_PACKET_SIZE+2];
    saved[3] = ptr[MAX_PHYSICAL_PACKET_SIZE+3];
    CRC crcobj;
    crcobj.update(ptr-2, MAX_PHYSICAL_PACKET_SIZE+2);
    uint32_t crc = crcobj.result();
    ptr[MAX_PHYSICAL_PACKET_SIZE] = crc >> 24;
    ptr[MAX_PHYSICAL_PACKET_SIZE+1] = crc >> 16;
    ptr[MAX_PHYSICAL_PACKET_SIZE+2] = crc >> 8;
    ptr[MAX_PHYSICAL_PACKET_SIZE+3] = crc;
    rawsend(ptr - HEADER_SPACE,
            MAX_PHYSICAL_PACKET_SIZE + HEADER_SPACE + FOOTER_SPACE);
    ptr[MAX_PHYSICAL_PACKET_SIZE] = saved[0];
    ptr[MAX_PHYSICAL_PACKET_SIZE+1] = saved[1];
    ptr[MAX_PHYSICAL_PACKET_SIZE+2] = saved[2];
    ptr[MAX_PHYSICAL_PACKET_SIZE+3] = saved[3];
    ptr += MAX_PHYSICAL_PACKET_SIZE;
    rem -= MAX_PHYSICAL_PACKET_SIZE;
  }
  // Now send the actual packet.
  ptr[-2] = packet_type;
  ptr[-1] = rem;
  CRC crcobj;
  crcobj.update(ptr-2, rem+2);
  uint32_t crc = crcobj.result();
  ptr[rem] = crc >> 24;
  ptr[rem+1] = crc >> 16;
  ptr[rem+2] = crc >> 8;
  ptr[rem+3] = crc;
  return rawsend(ptr - HEADER_SPACE, rem + HEADER_SPACE + FOOTER_SPACE);
}

bool PacketIO::recv(bool(*handler)(uint8_t, size_t, const uint8_t*)) {
  if(role != Role::Receiver) shutdown();
  auto ptr = buf;
  // end, not including CRC and optional phantom zero
  auto end_of_buf = ptr + HEADER_SPACE + MAX_LOGICAL_PACKET_SIZE;
  uint8_t packet_type;
  do {
    COBSInput in;
    packet_type = in.require();
    uint8_t length = in.require();
    if(length > MAX_PHYSICAL_PACKET_SIZE || (ptr + length) > end_of_buf)
      shutdown();
    if(packet_type == 0) {
      if(length == 0) {
        // Keepalive; fall through
      }
      else if(length == MAX_PHYSICAL_PACKET_SIZE) {
        // Fragment; fall through
      }
      else shutdown();
    }
    else if(packet_type == 0xFF) {
      // Echo Request. Let's send an Echo Response.
      if(length != 0) shutdown();
      Serial.write((const uint8_t[]){0,0,8}, 3);
      packet_type = 0;
      // length is already zero
    }
    uint8_t rem = length;
    while(rem-- > 0) {
      *ptr++ = in.require();
    }
    auto calculated_crc = in.final_crc();
    uint8_t crc[4];
    crc[0] = in.require();
    crc[1] = in.require();
    crc[2] = in.require();
    crc[3] = in.require();
    if((((uint32_t)(crc[0]) << 24U) | ((uint32_t)(crc[1]) << 16U)
        | ((uint32_t)(crc[2]) << 8U) | (uint32_t)(crc[3]))
       != calculated_crc)
      shutdown();
    switch(in.get()) {
    case 0: // phantom zero;
      if(in.get() != EOF)
        // fall through
    default:
        shutdown();
      // fall through
    case EOF: // natural end
      break;
    }
    if(packet_type == 0) {
      if(length == 0) {
        // Keepalive, do not acknowledge
      }
      else {
        // Fragment, acknowledge
        Serial.write((const uint8_t[]){0,0,2}, 3);
      }
    }
  } while(packet_type == 0);
  lastReceiveTime = millis();
  bool flip = handler(packet_type, ptr - buf, buf);
  if(flip) {
    Serial.write((const uint8_t[]){0,0,3}, 3);
    role = Role::Sender;
  }
  else {
    Serial.write((const uint8_t[]){0,0,1}, 3);
  }
  return flip;
}

void PacketIO::pumpHeart() {
  static bool askedForEcho = false;
  if(role != Role::Sender) shutdown();
  auto available = Serial.available();
  auto now = millis();
  if(available) {
    lastReceiveTime = now;
    if(available >= 3) {
      if(Serial.read() != 0) shutdown();
      if(Serial.read() != 0) shutdown();
      switch(Serial.read()) {
      case 7:
        // Heartbeat
        sendFromBuf(0, 0);
        break;
      case 8:
        // Echo response
        if(askedForEcho) {
          askedForEcho = false;
          break;
        }
        // fall through
      default:
        // something we did not expect...
        shutdown();
      }
    }
  }
  else if(lastReceiveTime + SOFT_TIMEOUT < now && !askedForEcho) {
    askedForEcho = true;
    Serial.write((const uint8_t[]){0x02,0xFF, 0x05,0xD2,0xFD,0xEF,0x8D, 0x00},
                 8);
  }
  else if(lastReceiveTime + HARD_TIMEOUT < now) {
    shutdown();
  }
}
