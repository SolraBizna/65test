#include <stdlib.h>
#include "w65c02.hh"

#include <iostream>
#include <algorithm>

#include <jsoncpp/json/json.h>

namespace {
  struct NoMore {};
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
  std::vector<Range> ranges;
  uint16_t serial_in_addr, serial_out_addr;
  std::string serial_in;
  size_t serial_in_pos = 0;
  std::string serial_out;
  const size_t MAX_SERIAL_OUT = 131072;
  enum class OutFormat {
    NONE, BASE64, UTF8
  } serial_out_fmt = OutFormat::NONE;
  bool serial_in_enabled = false, serial_out_enabled = false,
    last_pc_valid = false, vector_has_been_pulled = false,
    clear_so_next_cycle = false;
  uint16_t last_pc;
  uint32_t cycles_to_report = 0, cycles_to_run = 10000000, num_cycles = 5;
  std::vector<uint32_t> cycles;
  uint8_t terminate_on = uint8_t(~TERMINATE_ON_UNUSED_FLAGS);
  uint8_t termination_cause = 0;
  class System;
  extern W65C02::Core<System> cpu;
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
      case 1: cpu.set_so(state); break;
      case 2: cpu.set_nmi(state); break;
      case 3: cpu.set_irq(state); break;
      }
    }
  };
  std::vector<Flip> flips;
  size_t next_flip = 0;
  inline void report_cycle(uint8_t cycle_type, uint16_t addr, uint8_t data) {
    if(cycles_to_report > 0) {
      cycles.push_back((uint32_t(cycle_type) << 24)
                       | (uint32_t(addr) << 8)
                       | data);
      --cycles_to_report;
    }
    ++num_cycles;
    if(num_cycles == cycles_to_run) throw false;
    while(next_flip < flips.size()
          && flips[next_flip].getCycle() <= num_cycles) {
      flips[next_flip++].apply();
    }
  }
  class System {
    uint8_t raw_read_byte(uint16_t addr) {
      if(serial_in_enabled && addr == serial_in_addr) {
        if(serial_in_pos >= serial_in.length()) {
          clear_so_next_cycle = true;
          cpu.set_so(true);
          return 0;
        }
        else return uint8_t(serial_in[serial_in_pos++]);
      }
      else return sram[addr];
    }
  public:
    uint8_t read_opcode(uint16_t addr, W65C02::ReadType read_type) {
      return read_byte(addr, read_type);
    }
    uint8_t read_byte(uint16_t addr, W65C02::ReadType read_type) {
      using W65C02::ReadType;
      uint8_t data = raw_read_byte(addr);
      switch(read_type) {
      case ReadType::OPCODE:
      case ReadType::PREEMPTED:
        if(vector_has_been_pulled) {
          report_cycle(0xF, addr, data);
          if((terminate_on & TERMINATE_ON_INFINITE) && addr == last_pc
             && last_pc_valid) {
            termination_cause = 2;
            throw false;
          }
          last_pc_valid = true;
          last_pc = addr;
          if((terminate_on & TERMINATE_ON_ZERO) && addr < 0x0100) {
            termination_cause = 3;
            throw false;
          }
          if((terminate_on & TERMINATE_ON_STACK) && addr < 0x0200
             && addr >= 0x0100) {
            termination_cause = 4;
            throw false;
          }
          if((terminate_on & TERMINATE_ON_VECTOR) && addr >= 0xfffa) {
            termination_cause = 5;
            throw false;
          }
          if((terminate_on & TERMINATE_ON_BRK) && data == 0) {
            termination_cause = 1;
            throw false;
          }
        }
        break;
      case ReadType::DATA_LOCKED:
      case ReadType::IOP_LOCKED:
        if(vector_has_been_pulled) {
          report_cycle(0x3, addr, data);
        }
        break;
      default:
        if(vector_has_been_pulled) {
          report_cycle(0x7, addr, data);
        }
        break;
      }
      return data;
    }
    uint8_t fetch_vector_byte(uint16_t addr) {
      uint8_t data = raw_read_byte(addr);
      vector_has_been_pulled = true;
      report_cycle(0x5, addr, data);
      return data;
    }
    void write_byte(uint16_t addr, uint8_t data, W65C02::WriteType write_type){
      using W65C02::WriteType;
      switch(write_type) {
      case WriteType::DATA:
      case WriteType::PUSH:
        report_cycle(0x6, addr, data);
        break;
      case WriteType::DATA_LOCKED:
        report_cycle(0x2, addr, data);
        break;
      }
      if(serial_out_enabled && addr == serial_out_addr) {
        if(serial_out.length() >= MAX_SERIAL_OUT) {
          clear_so_next_cycle = true;
          cpu.set_so(true);
        }
        else serial_out.push_back(data);
      }
      else {
        bool valid = false;
        for(auto& range : ranges) {
          if(range.contains(addr)) {
            valid = true;
            break;
          }
        }
        if(valid) sram[addr] = data;
        else if(terminate_on & TERMINATE_ON_BAD_WRITE) {
          termination_cause = 6;
          throw false;
        }
      }
    }
  } system;
  W65C02::Core<System> cpu(system);
  uint8_t unbase64(char digit) {
    if(digit >= 'A' && digit <= 'Z') return digit - 'A';
    else if(digit >= 'a' && digit <= 'z') return digit - 'a' + 26;
    else if(digit >= '0' && digit <= '9') return digit - '0' + 52;
    else if(digit == '+') return 62;
    else if(digit == '/') return 63;
    // o_O!?
    else return 0;
  }
  std::string data_decode(const std::string& source) {
    if(source.find("utf8:") == 0) {
      auto ret = source;
      ret.erase(0, 5);
      return ret;
    }
    else if(source.find("base64:") == 0) {
      std::string ret;
      ret.reserve((source.length() - 7) * 3 / 4);
      // heh heh heh...
      auto p = source.begin() + 7;
      while(p < source.end()) {
        uint8_t a = unbase64(*p++);
        uint8_t b = unbase64(p >= source.end() ? '=' : *p++);
        int count = 3;
        if(p >= source.end() || *p == '=') count = 1;
        uint8_t c = unbase64(p >= source.end() ? '=' : *p++);
        if(p >= source.end() || *p == '=') count = 2;
        uint8_t d = unbase64(p >= source.end() ? '=' : *p++);
        ret.push_back((a << 2) | (b >> 4));
        if(count >= 2) {
          ret.push_back((b << 4) | (c >> 2));
          if(count >= 3) {
            ret.push_back((c << 6) | d);
          }
        }
      }
      return ret;
    }
    else {
      std::cerr << "Unknown data format\n";
      abort();
    }
  }
  void write_init_records(Json::Value& records) {
    for(unsigned int n = 0; n < records.size(); ++n) {
      auto& record = records[n];
      uint16_t i = record["base"].asUInt();
      auto data = data_decode(record["data"].asString());
      if(data.empty()) {
        std::cerr << "Empty init record\n";
        abort();
      }
      size_t j = 0;
      uint32_t rem;
      if(!!record["size"]) rem = record["size"].asUInt();
      else rem = data.length();
      while(rem-- > 0) {
        sram[i++] = data[j++];
        if(j >= data.length()) j = 0;
      }
    }
  }
  void add_flips(Json::Value& src, uint32_t flip_type) {
    bool state = false;
    std::vector<unsigned int> v;
    for(unsigned int n = 0; n < src.size(); ++n) {
      v.push_back(src[n].asUInt());
    }
    std::sort(v.begin(), v.end());
    flip_type <<= 24;
    for(auto cycle : v) {
      state = !state;
      flips.emplace_back((state ? 0x80000000 : 0)
                         | flip_type
                         | cycle);
    }
  }
}

int main() {
  Json::Value job;
  if(!(std::cin >> job)) {
    std::cerr << "Parsing the job failed\n";
    return 1;
  }
  sram[0xFFFD] = 0x02;
  write_init_records(job["init"]);
  if(!!job["rwmap"]) {
    auto& rwmap = job["rwmap"];
    for(unsigned int n = 0; n < rwmap.size(); ++n) {
      auto& raw = rwmap[n];
      ranges.emplace_back(raw[0].asUInt(), raw[1].asUInt());
    }
  }
  else {
    ranges.emplace_back(0x0000, 0x01FF);
  }
  if(!!job["serial_in_addr"]) {
    serial_in_addr = job["serial_in_addr"].asUInt();
    serial_in_enabled = true;
    if(!!job["serial_in_data"]) {
      serial_in = data_decode(job["serial_in_data"].asString());
    }
  }
  if(!!job["serial_out_addr"]) {
    serial_out_addr = job["serial_out_addr"].asUInt();
    serial_out_enabled = true;
  }
  if(!!job["serial_out_fmt"]) {
    auto fmt = job["serial_out_fmt"].asString();
    if(fmt == "utf8") {
      serial_out_fmt = OutFormat::UTF8;
    }
    else if(fmt == "base64") {
      serial_out_fmt = OutFormat::BASE64;
    }
    else {
      std::cerr << "Unknown serial_out_fmt\n";
      return 1;
    }
  }
  if(!!job["show_cycles"] && job["show_cycles"].asBool()) {
    cycles_to_report = 1000;
  }
  if(!!job["max_cycles"]) {
    cycles_to_run = job["max_cycles"].asUInt();
  }
  if(!!job["terminate_on_brk"] && !job["terminate_on_brk"].asBool()) {
    terminate_on &= ~TERMINATE_ON_BRK;
  }
  if(!!job["terminate_on_infinite_loop"] && !job["terminate_on_infinite_loop"].asBool()) {
    terminate_on &= ~TERMINATE_ON_INFINITE;
  }
  if(!!job["terminate_on_zero_fetch"] && !job["terminate_on_zero_fetch"].asBool()) {
    terminate_on &= ~TERMINATE_ON_ZERO;
  }
  if(!!job["terminate_on_stack_fetch"] && !job["terminate_on_stack_fetch"].asBool()) {
    terminate_on &= ~TERMINATE_ON_STACK;
  }
  if(!!job["terminate_on_vector_fetch"] && !job["terminate_on_vector_fetch"].asBool()) {
    terminate_on &= ~TERMINATE_ON_VECTOR;
  }
  if(!!job["terminate_on_bad_write"] && !job["terminate_on_bad_write"].asBool()) {
    terminate_on &= ~TERMINATE_ON_BAD_WRITE;
  }
  if(!!job["rdy"]) {
    std::cerr << "RDY signal is not supported\n";
    return 1;
  }
  if(!!job["res"]) {
    std::cerr << "reset signal is not supported\n";
    return 1;
  }
  if(!!job["so"]) add_flips(job["so"], 1);
  if(!!job["nmi"]) add_flips(job["nmi"], 2);
  if(!!job["irq"]) add_flips(job["irq"], 3);
  std::sort(flips.begin(), flips.end(),
            [](Flip& a, Flip& b) {
              return a.getCycle() < b.getCycle();
            });
  cpu.reset();
  try {
    while(num_cycles < cycles_to_run) {
      cpu.step();
      if(clear_so_next_cycle)
        cpu.set_so(false);
    }
  }
  // we terminate early with `throw true`
  catch(bool) {}
  Json::Value result(Json::ValueType::objectValue);
  if(last_pc_valid)
    result["last_pc"] = last_pc;
  result["num_cycles"] = num_cycles;
  switch(termination_cause) {
  case 0: result["termination_cause"] = "limit"; break;
  case 1: result["termination_cause"] = "brk"; break;
  case 2: result["termination_cause"] = "infinite_loop"; break;
  case 3: result["termination_cause"] = "zero_fetch"; break;
  case 4: result["termination_cause"] = "stack_fetch"; break;
  case 5: result["termination_cause"] = "vector_fetch"; break;
  case 6: result["termination_cause"] = "bad_write"; break;
  default: result["termination_cause"] = "unknown"; break;
  }
  if(!cycles.empty()) {
    Json::Value result_cycles(Json::ValueType::arrayValue);
    for(auto cycle : cycles) {
      char buf[8];
      snprintf(buf, 8, "%07X", cycle & 0xFFFFFFF);
      result_cycles.append(buf);
    }
    result["cycles"] = result_cycles;
  }
  switch(serial_out_fmt) {
  case OutFormat::NONE: break;
  case OutFormat::UTF8:
    // TODO: replace invalid UTF-8 with \u{FFFD}
    result["serial_out_data"] = std::string("utf8:")+serial_out;
    break;
  case OutFormat::BASE64: {
    static const char BASE64_DIGITS[64] = {
      'A','B','C','D','E','F','G','H','I','J','K','L','M','N','O','P','Q','R',
      'S','T','U','V','W','X','Y','Z',
      'a','b','c','d','e','f','g','h','i','j','k','l','m','n','o','p','q','r',
      's','t','u','v','w','x','y','z',
      '0','1','2','3','4','5','6','7','8','9',
      '+','/',
    };
    size_t len = serial_out.length();
    std::string encoded = "base64:";
    encoded.reserve((len + 2) / 3 * 4);
    size_t n = 0;
    while(n + 2 < len) {
      auto a = uint8_t(serial_out[n]);
      auto b = uint8_t(serial_out[n+1]);
      auto c = uint8_t(serial_out[n+2]);
      encoded.push_back(BASE64_DIGITS[a>>2]);
      encoded.push_back(BASE64_DIGITS[((a<<4)&0x30)|(b>>4)]);
      encoded.push_back(BASE64_DIGITS[((b<<2)&0x3C)|(c>>6)]);
      encoded.push_back(BASE64_DIGITS[c&0x3F]);
      n += 3;
    }
    switch(len - n) {
    case 0: break;
    case 1: {
      auto a = uint8_t(serial_out[n]);
      encoded.push_back(BASE64_DIGITS[a>>2]);
      encoded.push_back(BASE64_DIGITS[((a<<4)&0x30)]);
      encoded.push_back('=');
      encoded.push_back('=');
      break;
    }
    case 2: {
      auto a = uint8_t(serial_out[n]);
      auto b = uint8_t(serial_out[n+1]);
      encoded.push_back(BASE64_DIGITS[a>>2]);
      encoded.push_back(BASE64_DIGITS[((a<<4)&0x30)|(b>>4)]);
      encoded.push_back(BASE64_DIGITS[((b<<2)&0x3C)]);
      encoded.push_back('=');
      break;
    }
    }
    result["serial_out_data"] = encoded;
  } break;
  }
  std::cout << result;
  return 0;
}
