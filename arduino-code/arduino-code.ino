// -*- c++ -*-
#include "CPU.hh"
#include "PacketIO.hh"

void shutdown() {
  // Put the W65C02S into a safe, sane state
  CPU::rawClock(false);
  CPU::setOverflow(false);
  CPU::setNMI(false);
  CPU::setIRQ(false);
  CPU::setReady(false);
  CPU::setBE(false);
  // Flashy light! And each time we flash, fill the TX buffer with zeroes.
  // Thus, we indicate to the operator *and* the host that we're shutting down.
  for(int n = 0; n < 30; ++n) {
    while(Serial.availableForWrite() > 0)
      Serial.write(uint8_t(0));
    digitalWrite(13, LOW);
    delay(50);
    digitalWrite(13, HIGH);
    delay(50);
  }
  digitalWrite(13, HIGH);
  pmc_enable_backupmode();
  // SHOULD NOT BE REACHED
  // Flash the light (slowly) to indicate a problem
  while(true) {
    digitalWrite(13, LOW);
    delay(200);
    digitalWrite(13, HIGH);
    delay(200);
  }
}

// change to #if 1 to aid debugging unexpected shutdowns
#if 0
void shutdown2(int line) __attribute__((noreturn));
void shutdown2(int line) {
  Serial.write(uint8_t(0));
  Serial.write(uint8_t(0));
  Serial.write(uint8_t(line >> 24));
  Serial.write(uint8_t(line >> 16));
  Serial.write(uint8_t(line >> 8));
  Serial.write(uint8_t(line));
  shutdown();
}
#define shutdown() shutdown2(__LINE__)
#endif

const uint8_t TERMINATE_ON_BRK = 0x01;
const uint8_t TERMINATE_ON_INFINITE = 0x02;
const uint8_t TERMINATE_ON_ZERO = 0x04;
const uint8_t TERMINATE_ON_STACK = 0x08;
const uint8_t TERMINATE_ON_VECTOR = 0x10;
const uint8_t TERMINATE_ON_BAD_WRITE = 0x20;
const uint8_t TERMINATE_ON_UNUSED_FLAGS = 0xC0;

uint8_t sram[65536];
struct Range {
  uint16_t beg, end;
  Range() : beg(65535), end(0) {}
  Range(uint16_t beg, uint16_t end) : beg(beg), end(end) {}
  bool contains(uint16_t addr) const {
    return addr >= beg && addr <= end;
  }
};
const int MAX_RANGES = 8;
Range ranges[MAX_RANGES] = {
  Range(0x0000, 0x01FF),
  // remaining Ranges are invalid
};
int num_ranges = 1;
uint16_t serial_in_addr, serial_out_addr, write_addr = 0x0200;
#define last_pc write_addr
const int SERIAL_BUF_SIZE = 32;
uint8_t serial_in_buf[SERIAL_BUF_SIZE];
// serial_in_total gets set to EOF if there is no more
int serial_in_consumed = 0, serial_in_total = 0;
uint8_t serial_out_buf[SERIAL_BUF_SIZE];
int serial_out_size = 0, serial_out_rem = 131072;
bool serial_in_enabled = false, serial_out_enabled = false, terminated = false,
  last_pc_valid = false, vector_has_been_pulled = false,
  clear_so_next_cycle = false;
uint32_t max_cycles_to_report = 0, max_cycles = 10000000;
uint8_t terminate_on = uint8_t(~TERMINATE_ON_UNUSED_FLAGS);
uint8_t termination_cause = 0;
class Flip {
  uint32_t underlying;
public:
  Flip() : underlying(0) {}
  Flip(uint32_t underlying) : underlying(underlying) {}
  uint32_t getCycle() {
    return underlying & 0xFFFFFF;
  }
  bool getState() {
    return (underlying & 0x80000000) != 0;
  }
  int getPin() {
    return (underlying >> 24) & 0x7F;
  }
  void apply() {
    bool state = getState();
    switch(getPin()) {
    case 0: CPU::setReset(state); break;
    case 1: CPU::setOverflow(state); break;
    case 2: CPU::setNMI(state); break;
    case 3: CPU::setIRQ(state); break;
    case 4: CPU::setReady(state); break;
    case 5: CPU::setBE(state); break;
    default: shutdown();
    }
  }
};
const int MAX_FLIPS = 120;
Flip flips[MAX_FLIPS] = {};
int num_flips = 0, next_flip = 0;

void setup() {
  CPU::setup();
  // turn off that LED
  pinMode(13, OUTPUT);
  digitalWrite(13, LOW);
  // 115200 baud, 8 data bits, 1 stop bit, no parity.
  Serial.begin(115200, SERIAL_8N1);
  // Send the wakeup sequence
  Serial.write((const uint8_t[]){0,0,4,0,0,5,0,0,6}, 9);
  // Initialize the SRAM
  memset(sram, 0, sizeof(sram));
  sram[0xfffd] = 2;
}

bool serial_in_state(uint8_t packet_type, size_t length, const uint8_t* data) {
  auto ptr = data;
  switch(packet_type) {
  case 0x53: {
    // Serial input
    if(length > SERIAL_BUF_SIZE) shutdown();
    serial_in_consumed = 0;
    if(length == 0) {
      serial_in_total = EOF;
    }
    else {
      memcpy(serial_in_buf, ptr, length);
      serial_in_total = length;
    }
    return true;
  }
  default: shutdown();
  }
}

bool start_state(uint8_t packet_type, size_t length, const uint8_t* data) {
  auto ptr = data;
  switch(packet_type) {
  case 0x01: {
    // Initialization record.
    if(length == 0) shutdown();
    while(length > 0) {
      auto amt = std::min(length, size_t(65536 - write_addr));
      memcpy(sram + write_addr, ptr, amt);
      ptr += amt;
      write_addr += amt;
      length -= amt;
    }
    return false;
  }
  case 0x02: {
    // Writable memory ranges.
    if(length % 4 != 0 || length > MAX_RANGES * 4) shutdown();
    num_ranges = length / 4;
    for(int n = 0; n < num_ranges; ++n) {
      ranges[n] = Range((ptr[0] << 8U) | ptr[1], (ptr[2] << 8U) | ptr[3]);
      ptr += 4;
    }
    return false;
  }
  case 0x03: {
    // serial in port
    if(length != 2) shutdown();
    serial_in_addr = (ptr[0] << 8U) | ptr[1];
    serial_in_enabled = true;
    return false;
  }
  case 0x04: {
    // serial out port
    if(length != 2) shutdown();
    serial_out_addr = (ptr[0] << 8U) | ptr[1];
    serial_out_enabled = true;
    return false;
  }
  case 0x05: {
    // max cycles to report
    if(length != 4) shutdown();
    max_cycles_to_report = (ptr[0] << 24U) | (ptr[1] << 16U)
      | (ptr[2] << 8U) | ptr[3];
    return false;
  }
  case 0x06: {
    // max cycles to RUN
    if(length != 4) shutdown();
    max_cycles = (ptr[0] << 24U) | (ptr[1] << 16U) | (ptr[2] << 8U) | ptr[3];
    return false;
  }
  case 0x07: {
    // termination flags
    if(length != 1 || (ptr[0] & TERMINATE_ON_UNUSED_FLAGS) != 0) shutdown();
    terminate_on = ptr[0];
    return false;
  }
  case 0x08: {
    // flag changes
    if(length == 0 || length > MAX_FLIPS * 4 || length % 4 != 0) shutdown();
    num_flips = length / 4;
    for(int n = 0; n < num_flips; ++n) {
      Flip flip((ptr[0] << 24U) | (ptr[1] << 16U) | (ptr[2] << 8U) | ptr[3]);
      if(flip.getPin() > 5) shutdown();
      ptr += 4;
      flips[n] = flip;
    }
    return false;
  }
  case 0x09: {
    // init write pos
    if(length != 2) shutdown();
    write_addr = (data[0] << 8U) | data[1];
    return false;
  }
  case 0xFE: {
    // Go!
    return true;
  }
  default: shutdown();
  }
}

uint8_t* buf;
// The current position in a partially-transmitted cycle report, if any
uint8_t* ptr;

inline void lowPhase() {
  CPU::rawClock(false);
  if(clear_so_next_cycle) {
    clear_so_next_cycle = false;
    CPU::setOverflow(false);
  }
}

uint8_t highPhase(uint32_t bus_state = CPU::readABusRaw()) {
  uint16_t addr;
  bool rwb, mlb, vpb, sync;
  CPU::cookABus(bus_state, addr, rwb, vpb, mlb, sync);
  if(rwb) {
    if(sync) {
      if((terminate_on & TERMINATE_ON_INFINITE)
         && last_pc == addr
         && last_pc_valid) {
        termination_cause = 0x02; // infinite loop
        terminated = true;
      }
      if(vector_has_been_pulled) {
        if(addr < 0x100 && (terminate_on & TERMINATE_ON_ZERO)) {
          termination_cause = 0x03; // zero page instruction fetch
          terminated = true;
        }
        else if(addr >= 0x100 && addr < 0x200
                && (terminate_on & TERMINATE_ON_STACK)) {
          termination_cause = 0x04; // stack page instruction fetch
          terminated = true;
        }
        else if(addr >= 0xfffa && (terminate_on & TERMINATE_ON_VECTOR)) {
          termination_cause = 0x05; // vector instruction fetch
          terminated = true;
        }
        last_pc = addr;
        last_pc_valid = true;
      }
    }
    else if(vpb) {
      vector_has_been_pulled = true;
    }
    uint8_t data;
    if(serial_in_enabled && addr == serial_in_addr) {
      if(serial_in_total >= 0) {
        if(serial_in_consumed >= serial_in_total) {
          if(ptr - buf > 0) {
            // send a cycle report first
            if(PacketIO::sendFromBuf(0x01, ptr - buf))
              shutdown(); // Must not flip
            ptr = buf;
          }
          if(!PacketIO::sendFromBuf(0x02, 0)) // Serial read request
            shutdown(); // Must flip
          if(!PacketIO::recv(serial_in_state))
            shutdown(); // Must flip back
        }
      }
      if(serial_in_total < 0) {
        data = 0;
        CPU::setOverflow(true);
        clear_so_next_cycle = true;
      }
      else {
        data = serial_in_buf[serial_in_consumed++];
      }
    }
    else {
      data = sram[addr];
    }
    CPU::writeDataAdvancingClock(data);
    if(vector_has_been_pulled && sync && data == 0
       && (terminate_on & TERMINATE_ON_BRK) && !terminated) {
      termination_cause = 0x01; // BRK
      terminated = true;
    }
    return data;
  }
  else {
    CPU::rawClock(true);
    uint8_t data = CPU::readData();
    if(serial_out_enabled && addr == serial_out_addr) {
      if(serial_out_rem == 0) {
        CPU::setOverflow(true);
        clear_so_next_cycle = true;
      }
      else {
        --serial_out_rem;
        serial_out_buf[serial_out_size++] = data;
        if(serial_out_size >= SERIAL_BUF_SIZE) {
          if(ptr - buf > 0) {
            // send a cycle report first
            if(PacketIO::sendFromBuf(0x01, ptr - buf))
              shutdown(); // must not flip
            ptr = buf;
          }
          memcpy(buf, serial_out_buf, serial_out_size);
          if(PacketIO::sendFromBuf(0x03, serial_out_size))
            shutdown(); // must not flip
          serial_out_size = 0;
        }
      }
    }
    else {
      bool valid = false;
      for(int n = 0; n < num_ranges; ++n) {
        if(ranges[n].contains(addr)) {
          valid = true;
          break;
        }
      }
      if(valid) {
        sram[addr] = data;
      }
      else if(terminate_on & TERMINATE_ON_BAD_WRITE) {
        termination_cause = 0x06; // bad write
        terminated = true;
      }
    }
    return data;
  }
}

void cook_bus_state(uint32_t bus_state, uint8_t data, uint8_t* ptr) {
  // cycle type
  ptr[0] = ((bus_state >> 9) & 1) | ((bus_state >> 20) & 14);
  // address
  ptr[1] = ((bus_state >> 12) & 0xFF);
  ptr[2] = ((bus_state >> 1) & 0xFF);
  // data byte
  ptr[3] = data;
}

void report_bus_error(uint32_t mask, uint32_t want, uint32_t bus, uint8_t cycle, uint8_t edge) {
  // super secret bus error report!
  uint8_t report[19] = {
    // we died!
    0, 0, 0xFF, 0, 0xFF, 0, 0xFF
  };
  cook_bus_state(mask, 0x00, report + 7);
  cook_bus_state(want, 0x00, report + 10);
  cook_bus_state(bus, 0x00, report + 13);
  report[16] = cycle;
  report[17] = edge;
  report[18] = 0xDE;
  Serial.write(report, sizeof(report));
}

void check_bus_cycle(uint32_t mask, uint32_t want, uint8_t cycle) {
  CPU::rawClock(false);
  auto bus = CPU::readABusRaw();
  if((bus & mask) != want) {
    report_bus_error(mask, want, bus, cycle, 0);
    shutdown();
  }
  CPU::rawClock(true);
  bus = CPU::readABusRaw();
  if((bus & mask) != want) {
    report_bus_error(mask, want, bus, cycle, 1);
    shutdown();
  }
}

void loop() {
  CPU::reset();
  // One last dummy cycle
  check_bus_cycle(0, 0, 0);
  CPU::setBE(true);
  // Cycle 0: SYNC read, don't care address
  check_bus_cycle(CPU::BUS_MASK(0), CPU::BUS_READ_SYNC(0), 0);
  // Cycle 1: unsync read, don't care address
  check_bus_cycle(CPU::BUS_MASK(0), CPU::BUS_READ(0), 1);
  // Cycle 2,3,4: fake pushes
  // if we're too picky about cycle 2, sometimes we get spurious failures
  check_bus_cycle(CPU::BUS_MASK(0xFF00), CPU::BUS_READ(0x0100), 2);
  check_bus_cycle(CPU::BUS_MASK(0xFF00), CPU::BUS_READ(0x0100), 3);
  check_bus_cycle(CPU::BUS_MASK(0xFF00), CPU::BUS_READ(0x0100), 4);
  while(!PacketIO::recv(start_state))
    ; // repeat until we flip
  // now in the Running state
  buf = PacketIO::getBuf();
  ptr = buf;
  uint32_t total_cycles = 5;
  uint32_t rem_cycles = max_cycles - 5;
  uint32_t rem_cycles_to_report = max_cycles_to_report;
  if(rem_cycles_to_report > rem_cycles) {
    rem_cycles_to_report = rem_cycles;
    rem_cycles = 0;
  }
  else {
    rem_cycles -= rem_cycles_to_report;
  }
  uint32_t whenNextFlip = num_flips == 0 ? uint32_t(0)-1
    : flips[0].getCycle();
  uint32_t start_time = millis();
#if USING_INADEQUATE_INTERNAL_PULLUP
  uint32_t slow_cycles = 0;
#endif
  // here and unfolded instead of inlined because speed!
  if(rem_cycles_to_report > 0) {
    while(rem_cycles_to_report > 0 && !terminated) {
      --rem_cycles_to_report;
      while(whenNextFlip <= total_cycles && next_flip < num_flips) {
        auto& flip = flips[next_flip];
        flip.apply();
#if USING_INADEQUATE_INTERNAL_PULLUP
        slow_cycles = 5;
#endif
        ++next_flip;
        if(next_flip < num_flips)
          whenNextFlip = flips[next_flip].getCycle();
        else
          break;
      }
#if USING_INADEQUATE_INTERNAL_PULLUP
      // give the pullup time to work... -_-
      if(slow_cycles > 0) {
        --slow_cycles;
        delay(5);
      }
#endif
      if((++total_cycles & 0x1F) == 0) PacketIO::pumpHeart();
      lowPhase();
      uint32_t bus_state = CPU::readABusRaw();
      uint8_t data = highPhase(bus_state);
      cook_bus_state(bus_state, data, ptr);
      ptr += 4;
      if(ptr - buf >= intptr_t(PacketIO::MAX_PHYSICAL_PACKET_SIZE-3)) {
        if(PacketIO::sendFromBuf(0x01, ptr - buf)) // cycle report
          shutdown(); // must not flip
        ptr = buf;
      }
    }
    if(ptr - buf > 0) {
      if(PacketIO::sendFromBuf(0x01, ptr - buf)) // our last cycle report
         shutdown(); // must not flip
      ptr = buf;
    }
  }
  while(rem_cycles > 0
#if USING_INADEQUATE_INTERNAL_PULLUP
        && (whenNextFlip != uint32_t(0)-1 || slow_cycles > 0)
#else
        && whenNextFlip != uint32_t(0)-1
#endif
        && !terminated) {
    while(whenNextFlip <= total_cycles && next_flip < num_flips) {
      auto& flip = flips[next_flip];
      flip.apply();
#if USING_INADEQUATE_INTERNAL_PULLUP
        slow_cycles = 5;
#endif
      ++next_flip;
      if(next_flip < num_flips)
        whenNextFlip = flips[next_flip].getCycle();
      else
        break;
    }
    --rem_cycles;
#if USING_INADEQUATE_INTERNAL_PULLUP
    // give the pullup time to work... -_-
    if(slow_cycles > 0) {
      --slow_cycles;
      delay(5);
    }
#endif
    if((++total_cycles & 0x1F) == 0) PacketIO::pumpHeart();
    lowPhase();
    highPhase();
  }
  while(rem_cycles > 0 && !terminated) {
    --rem_cycles;
    if((++total_cycles & 0x1FFFF) == 0) PacketIO::pumpHeart();
    lowPhase();
    highPhase();
  }
  // all done!
  // assert(ptr == buf)
  if(serial_out_enabled && serial_out_size > 0) {
    memcpy(buf, serial_out_buf, serial_out_size);
    if(PacketIO::sendFromBuf(0x03, serial_out_size))
      shutdown(); // must not flip
  }
  auto runtime = millis() - start_time;
  buf[0] = total_cycles >> 24;
  buf[1] = total_cycles >> 16;
  buf[2] = total_cycles >> 8;
  buf[3] = total_cycles;
  buf[4] = runtime >> 24;
  buf[5] = runtime >> 16;
  buf[6] = runtime >> 8;
  buf[7] = runtime;
  buf[8] = last_pc >> 8;
  buf[9] = last_pc;
  buf[10] = termination_cause;
  PacketIO::sendFromBuf(0x04, 11); // termination
  shutdown();
}
