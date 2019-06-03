#ifndef CPUHH
#define CPUHH

// set this to 0 if you have hooked a 3.3kΩ resistor between 3.3V and RDY.
#define USING_INADEQUATE_INTERNAL_PULLUP 1

// (we can't make our pins namespaced consts without running into some
// #defines in the system libraries...)

// these pins are on port C
#define CPU_A0 33 // PC1
#define CPU_A1 34 // PC2
#define CPU_A2 35 // PC3
#define CPU_A3 36 // PC4
#define CPU_A4 37 // PC5
#define CPU_A5 38 // PC6
#define CPU_A6 39 // PC7
#define CPU_A7 40 // PC8
#define CPU_A8 51 // PC12
#define CPU_A9 50 // PC13
#define CPU_A10 49 // PC14
#define CPU_A11 48 // PC15
#define CPU_A12 47 // PC16
#define CPU_A13 46 // PC17
#define CPU_A14 45 // PC18
#define CPU_A15 44 // PC19
#define CPU_RWB 41 // PC9
#define CPU_VPB 9 // PC21
#define CPU_MLB 8 // PC22
#define CPU_SYNC 7 // PC23
// these pins are on port D
#define CPU_D0 25 // PD0
#define CPU_D1 26 // PD1
#define CPU_D2 27 // PD2
#define CPU_D3 28 // PD3
#define CPU_D4 14 // PD4
#define CPU_D5 15 // PD5
#define CPU_D6 29 // PD6
#define CPU_D7 11 // PD7
#define CPU_PHI2 12 // PD8
// the remaining six pins are accessed via digialWrite/etc. and can be put on
// whatever pins you want
#define CPU_RESB 2
#define CPU_SOB 3
#define CPU_BE 4
#define CPU_RDY 10
#define CPU_NMIB 5
#define CPU_IRQB 6

namespace CPU {
  // will be low after reset
  static inline void rawClock(bool v) {
    if(v) {
      REG_PIOD_SODR = 0x100;
      REG_PIOD_SODR = 0x100;
      REG_PIOD_SODR = 0x100;
      REG_PIOD_SODR = 0x100;
    }
    else {
      REG_PIOD_CODR = 0x100;
      REG_PIOD_CODR = 0x100;
      REG_PIOD_CODR = 0x100;
      REG_PIOD_CODR = 0x100;
    }
    //digitalWrite(CPU_PHI2, v);
  }
  // edge triggered, default false
  static inline void setOverflow(bool v) {
    digitalWrite(CPU_SOB, !v);
  }
  // edge triggered, default false
  static inline void setNMI(bool v) {
    digitalWrite(CPU_NMIB, !v);
  }
  // level triggered, default false
  static inline void setIRQ(bool v) {
    digitalWrite(CPU_IRQB, !v);
  }
  // level triggered, default true, bidirectional (wired AND)
  static inline void setReady(bool v) {
    if(v) {
      pinMode(CPU_RDY, USING_INADEQUATE_INTERNAL_PULLUP?INPUT_PULLUP:INPUT);
    }
    else {
      pinMode(CPU_RDY, OUTPUT);
      digitalWrite(CPU_RDY, LOW);
    }
  }
  // TODO: does this work if we're outputting LOW? It should...
  static inline bool getReady() {
    return digitalRead(CPU_RDY);
  }
  // level triggered, default false
  static inline void setBE(bool v) {
    digitalWrite(CPU_BE, v);
  }
  // you shouldn't change this yourself...
  static inline void setReset(bool v) {
    digitalWrite(CPU_RESB, !v);
  }

  static inline uint32_t readABusRaw() {
    return REG_PIOC_PDSR;
  }
  static inline void cookABus(uint32_t bus, uint16_t& addr, bool& rwb, bool& vpb, bool& mlb, bool& sync) {
    addr = ((bus >> 1) & 0xFF) | (((bus >> 4) & 0xFF00));
    rwb = !!(bus & 0x200);
    vpb = !(bus & (1<<21));
    mlb = !(bus & (1<<22));
    sync = !!(bus & (1<<23));
  }
  static inline void readABus(uint16_t& addr, bool& rwb, bool& vpb, bool& mlb, bool& sync) {
    cookABus(readABusRaw(), addr, rwb, vpb, mlb, sync);
  }
  static inline uint32_t BUS(uint16_t addr, bool rwb, bool vpb, bool mlb, bool sync) {
    uint32_t ret = ((addr & 0xFF) << 1) | ((addr >> 8) << 12);
    if(rwb) ret |= 0x200;
    if(!vpb) ret |= (1<<21);
    if(!mlb) ret |= (1<<22);
    if(sync) ret |= (1<<23);
    return ret;
  }
  static inline uint32_t BUS_MASK(uint16_t addr) { return BUS(addr, true, false, false, true); }
  static inline uint32_t BUS_WRITE(uint16_t addr) { return BUS(addr, false, false, false, false); }
  static inline uint32_t BUS_WRITE_MLB(uint16_t addr) { return BUS(addr, false, false, true, false); }
  static inline uint32_t BUS_READ(uint16_t addr) { return BUS(addr, true, false, false, false); }
  static inline uint32_t BUS_READ_MLB(uint16_t addr) { return BUS(addr, true, false, true, false); }
  static inline uint32_t BUS_READ_SYNC(uint16_t addr) { return BUS(addr, true, false, false, true); }
  static inline uint32_t BUS_READ_VPB(uint16_t addr) { return BUS(addr, true, true, false, false); }
  static inline uint8_t readData() {
    return REG_PIOD_PDSR;
  }
  static inline void writeDataAdvancingClock(uint8_t d) {
    uint32_t w = d | 0x100;
    // D0-D7 to write mode... temporarily
    REG_PIOD_OER = 0xFF;
    REG_PIOD_ODSR = w;
    REG_PIOD_ODSR = w;
    REG_PIOD_ODSR = w;
    REG_PIOD_ODSR = w;
    // let the lines drain
    REG_PIOD_ODR = 0xFF;
  }
  void reset() {
    setReset(true);
    for(int n = 0; n < 2; ++n) {
      rawClock(false);
      rawClock(true);
    }
    setReset(false);
  }
  // Call this IMMEDIATELY from the sketch's top-level setup()!
  void setup() {
    pinMode(CPU_BE, OUTPUT);
    setBE(false);
    //dataReadMode();
    pinMode(CPU_RDY, USING_INADEQUATE_INTERNAL_PULLUP?INPUT_PULLUP:INPUT);
    //pinMode(CPU_RWB, INPUT);
    //pinMode(CPU_VPB, INPUT);
    //pinMode(CPU_MLB, INPUT);
    //pinMode(CPU_SYNC, INPUT);
    pinMode(CPU_RESB, OUTPUT);
    pinMode(CPU_SOB, OUTPUT);
    //pinMode(CPU_PHI2, OUTPUT);
    pinMode(CPU_BE, OUTPUT);
    pinMode(CPU_NMIB, OUTPUT);
    pinMode(CPU_IRQB, OUTPUT);
    pmc_enable_periph_clk(ID_PIOC);
    pmc_enable_periph_clk(ID_PIOD);
    // A0-A15, RWB, VPB, MLB, SYNC input
    REG_PIOC_ODR = 0b111011111111001111111110;
    // put D0-D7 and PHI2 into PIO line mode
    REG_PIOD_PER = 0b111111111;
    // D0-D7 input, PHI2 output
    REG_PIOD_ODR = 0b11111111;
    REG_PIOD_OER = 0b100000000;
    // safe the pins other than clock and D
    REG_PIOD_OWER = 0b111111111;
    setOverflow(false);
    setNMI(false);
    setIRQ(false);
    setReady(true);
    reset();
  }

}

#endif
