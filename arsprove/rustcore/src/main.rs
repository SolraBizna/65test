#![feature(try_blocks)]

extern crate base64;
extern crate serde;
extern crate serde_json;
#[macro_use]
extern crate serde_derive;
extern crate w65c02s;

use serde::de::Error as SerdeDeError;
use serde::de;
use serde_json::Value;
use std::borrow::BorrowMut;
use std::clone::Clone;
use std::cmp::Ordering;
use std::collections::VecDeque;
use std::fmt;
use std::io::BufRead;
use std::ops::Deref;
use w65c02s::{W65C02S, P_V};

const MIN_CYCLE_COUNT: u32 = 9;
const MAX_CYCLE_COUNT: u32 = 10000000;
const MAX_SPECIAL_CYCLES: usize = 20;
const CYCLES_TO_REPORT: u32 = 1000;

#[derive(Debug)]
enum DataType {
    Utf8, Base64
}
impl<'de> serde::Deserialize<'de> for DataType {
    fn deserialize<D>(deserializer: D) -> Result<DataType, D::Error>
    where D: serde::Deserializer<'de> {
        deserializer.deserialize_str(DataTypeVisitor)
    }
}
struct DataTypeVisitor;
impl<'de> de::Visitor<'de> for DataTypeVisitor {
    type Value = DataType;
    fn expecting(&self, formatter: &mut fmt::Formatter) -> fmt::Result {
        formatter.write_str("either utf8 or base64")
    }
    fn visit_str<E: de::Error>(self, s: &str) -> Result<DataType, E> {
        if s == "base64" { Ok(DataType::Base64) }
        else if s == "utf8" { Ok(DataType::Utf8) }
        else {
            Err(E::custom("expected either utf8 or base64"))
        }
    }
}

#[derive(Deserialize, Debug)]
struct InitRec {
    base: u16,
    data: Blob,
    size: Option<u32>,
}

#[derive(Debug)]
struct Range {
    start: u16,
    end: u16
}
impl<'de> serde::Deserialize<'de> for Range {
    fn deserialize<D>(deserializer: D) -> Result<Range, D::Error>
    where D: serde::Deserializer<'de> {
        deserializer.deserialize_seq(RangeVisitor)
    }
}
struct RangeVisitor;
impl<'de> de::Visitor<'de> for RangeVisitor {
    type Value = Range;
    fn expecting(&self, formatter: &mut fmt::Formatter) -> fmt::Result {
        formatter.write_str("an array [a,b] where a are valid 16-bit addresses and b is >= a")
    }
    fn visit_seq<A>(self, mut seq: A) -> Result<Range, A::Error>
    where A: de::SeqAccess<'de> {
        let len = seq.size_hint().expect("array lengths should be known");
        if len != 2 {
            return Err(de::Error::custom("range must be 2-element array"));
        }
        let start = seq.next_element()?.unwrap();
        let end = seq.next_element()?.unwrap();
        if end < start {
            return Err(de::Error::custom("end of range cannot be less than \
                                          start of range"));
        }
        Ok(Range { start, end })
    }
}

#[derive(Debug)]
struct Blob {
    data: Vec<u8>
}
impl Deref for Blob {
    type Target = [u8];
    fn deref(&self) -> &[u8] { &self.data[..] }
}
impl<'de> serde::Deserialize<'de> for Blob {
    fn deserialize<D>(deserializer: D) -> Result<Blob, D::Error>
    where D: serde::Deserializer<'de> {
        deserializer.deserialize_str(BlobVisitor)
    }
}
struct BlobVisitor;
impl<'de> de::Visitor<'de> for BlobVisitor {
    type Value = Blob;
    fn expecting(&self, formatter: &mut fmt::Formatter) -> fmt::Result {
        formatter.write_str("a string starting with utf8: or base64:")
    }
    fn visit_str<E: de::Error>(self, s: &str) -> Result<Blob, E> {
        if s.starts_with("base64:") {
            match base64::decode(&s[7..]) {
                Ok(data) => Ok(Blob{data}),
                Err(_) => Err(E::custom("invalid base64 data")),
            }
        }
        else if s.starts_with("utf8:") {
            Ok(Blob{data: s[5..].as_bytes().to_vec()})
        }
        else {
            Err(E::custom("binary data must start with utf8: or base64:"))
        }
    }
    fn visit_byte_buf<E: de::Error>(self, data: Vec<u8>) -> Result<Blob, E> {
        Ok(Blob{data})
    }
}

#[derive(Deserialize, Debug)]
struct Job {
    init: Vec<InitRec>,
    rwmap: Option<Vec<Range>>,
    serial_in_addr: Option<u16>,
    serial_out_addr: Option<u16>,
    serial_in_data: Option<Blob>,
    serial_out_fmt: Option<DataType>,
    show_cycles: Option<bool>,
    max_cycles: Option<u32>,
    terminate_on_brk: Option<bool>,
    terminate_on_infinite_loop: Option<bool>,
    terminate_on_zero_fetch: Option<bool>,
    terminate_on_stack_fetch: Option<bool>,
    terminate_on_vector_fetch: Option<bool>,
    terminate_on_bad_write: Option<bool>,
    nmi: Option<Vec<u32>>,
    irq: Option<Vec<u32>>,
    rdy: Option<Vec<u32>>,
    so: Option<Vec<u32>>,
    res: Option<Vec<u32>>,
}

fn get_job(input: &mut BufRead) -> Result<Job, serde_json::Error> {
    try {
        let mut job: Job = serde_json::from_reader(input.borrow_mut())?;
        for rec in &job.init {
            let size = match rec.size {
                None => rec.data.len() as u32,
                Some(size) => size,
            };
            let endut = (rec.base as u32).saturating_add(size);
            if endut > 65536 {
                Err(serde_json::Error::custom("Initialization record exceeds \
                                               size of address space"))?
            }
        }
        if let Some(max_cycles) = job.max_cycles {
            if max_cycles > MAX_CYCLE_COUNT || max_cycles < MIN_CYCLE_COUNT {
                Err(serde_json::Error::custom("Maximum cycle count cannot \
                                               exceed 10,000,000 or be less \
                                               than 9"))?
            }
        }
        for spec in [&mut job.nmi, &mut job.irq, &mut job.rdy,
                     &mut job.so, &mut job.res].iter_mut() {
            if let Some(ref mut spec) = spec {
                if spec.len() > MAX_SPECIAL_CYCLES {
                    Err(serde_json::Error::custom("A given special signal may \
                                                   not toggle more than 20 \
                                                   times"))?
                }
                spec.sort_unstable();
            }
        }
        job
    }
}

#[derive(Clone,Copy,PartialEq,Eq)]
enum FlipType { Overflow, Nmi, Irq }

#[derive(PartialEq,Eq)]
struct Flip {
    typ: FlipType,
    cycle: u32,
    state: bool,
}

impl PartialOrd for Flip {
    fn partial_cmp(&self, other: &Flip) -> Option<Ordering> {
        Some(self.cmp(other))
    }
}

impl Ord for Flip {
    fn cmp(&self, other: &Flip) -> Ordering {
        self.cycle.cmp(&other.cycle)
    }
}

struct System {
    sram: [u8; 65536],
    writable: [bool; 65536],
    serial_in_addr: Option<u16>,
    serial_out_addr: Option<u16>,
    serial_in_data: VecDeque<u8>,
    serial_out_data: Vec<u8>,
    vector_has_been_pulled: bool,
    cycles_to_report: u32,
    cycles: Vec<String>,
    terminate_on_brk: bool,
    terminate_on_infinite_loop: bool,
    terminate_on_zero_fetch: bool,
    terminate_on_stack_fetch: bool,
    terminate_on_vector_fetch: bool,
    terminate_on_bad_write: bool,
    last_pc: Option<u16>,
    termination_cause: Option<&'static str>,
    cycles_to_run: u32,
    num_cycles: u32,
    flips: VecDeque<Flip>,
}

impl System {
    fn add_flips(flips: &mut Vec<Flip>, vec: &Option<Vec<u32>>, typ: FlipType) {
        if let Some(ref vec) = vec {
            let mut next_state = true;
            for cycle in vec.iter() {
                flips.push(Flip { cycle: *cycle, typ, state: next_state });
                next_state = !next_state;
            }
        }
    }
    pub fn new(job: &Job) -> System {
        if job.rdy.is_some() { panic!("RDY is not supported") }
        if job.res.is_some() { panic!("Reset is not supported") }
        let mut sram = [0; 65536];
        sram[0xFFFD] = 0x02;
        for rec in job.init.iter() {
            let base = rec.base as usize;
            let size = rec.size.map(|x| x as usize)
                .unwrap_or(rec.data.data.len());
            (&mut sram[base .. base+size]).copy_from_slice(&rec.data.data[..]);
        }
        let mut writable = [false; 65536];
        let rwmap = &job.rwmap;
        let rwmap = match rwmap {
            Some(rwmap) => &rwmap[..],
            None => &[Range { start: 0, end: 511 }],
        };
        for range in rwmap.iter() {
            for cell in (&mut writable[range.start as usize .. range.end as usize + 1]).iter_mut() {
                *cell = true;
            }
        }
        let serial_in_data = match &job.serial_in_data {
            Some(data) => data.data.clone().into(),
            None => VecDeque::new(),
        };
        let cycles_to_run = job.max_cycles.unwrap_or(MAX_CYCLE_COUNT);
        let mut flips = Vec::new();
        Self::add_flips(&mut flips, &job.so, FlipType::Overflow);
        Self::add_flips(&mut flips, &job.nmi, FlipType::Nmi);
        Self::add_flips(&mut flips, &job.irq, FlipType::Irq);
        flips.sort_unstable();
        System {
            sram, writable,
            serial_in_addr: job.serial_in_addr,
            serial_out_addr: job.serial_out_addr,
            serial_in_data,
            serial_out_data: Vec::new(),
            vector_has_been_pulled: false,
            cycles_to_run,
            cycles_to_report: if job.show_cycles.unwrap_or(false) { CYCLES_TO_REPORT.min(cycles_to_run) } else { 0 },
            cycles: Vec::new(),
            terminate_on_brk: job.terminate_on_brk.unwrap_or(true),
            terminate_on_infinite_loop: job.terminate_on_infinite_loop.unwrap_or(true),
            terminate_on_zero_fetch: job.terminate_on_zero_fetch.unwrap_or(true),
            terminate_on_stack_fetch: job.terminate_on_stack_fetch.unwrap_or(true),
            terminate_on_vector_fetch: job.terminate_on_vector_fetch.unwrap_or(true),
            terminate_on_bad_write: job.terminate_on_bad_write.unwrap_or(true),
            last_pc: None,
            termination_cause: None,
            num_cycles: 5,
            flips: flips.into(),
        }
    }
    fn report_cycle(&mut self, cpu: &mut W65C02S, typ: u32, addr: u16, data: u8) {
        if self.num_cycles >= self.cycles_to_run || self.termination_cause.is_some() { return }
        if self.cycles_to_report > 0 && self.termination_cause.is_none() {
            self.cycles_to_report -= 1;
            self.cycles.push(format!("{:1X}{:04X}{:02X}", typ, addr, data));
        }
        self.num_cycles += 1;
        while !self.flips.is_empty() {
            if self.flips.front().unwrap().cycle > self.num_cycles { break }
            let flip = self.flips.pop_front().unwrap();
            match flip.typ {
                FlipType::Overflow => if flip.state {
                    cpu.set_p(cpu.get_p() | P_V);
                },
                FlipType::Irq => cpu.set_irq(flip.state),
                FlipType::Nmi => cpu.set_nmi(flip.state),
            }
        }
    }
    fn handle_read(&mut self, cpu: &mut W65C02S, addr: u16) -> u8 {
        if let Some(serial_in_addr) = self.serial_in_addr {
            if addr == serial_in_addr {
                match self.serial_in_data.pop_front() {
                    Some(x) => return x,
                    None => {
                        cpu.set_p(cpu.get_p() | P_V);
                        return 0;
                    }
                }
            }
        }
        self.sram[addr as usize]
    }
    fn handle_write(&mut self, _: &mut W65C02S, addr: u16, value: u8) {
        if let Some(serial_out_addr) = self.serial_out_addr {
            if addr == serial_out_addr {
                return self.serial_out_data.push(value);
            }
        }
        if self.writable[addr as usize] {
            self.sram[addr as usize] = value;
        }
        else if self.terminate_on_bad_write && self.termination_cause.is_none() {
            self.termination_cause = Some("bad_write");
        }
    }
    fn perform_read(&mut self, cpu: &mut W65C02S, typ: u32, addr: u16) -> u8 {
        let ret = self.handle_read(cpu, addr);
        if self.vector_has_been_pulled {
            self.report_cycle(cpu, typ, addr, ret)
        }
        ret
    }
    fn perform_write(&mut self, cpu: &mut W65C02S, typ: u32, addr: u16, value: u8) {
        if self.vector_has_been_pulled {
            self.report_cycle(cpu, typ, addr, value)
        }
        self.handle_write(cpu, addr, value);
    }
}

const LOCKED_WRITE: u32 = 2;
const LOCKED_READ: u32 = 3;
const VECTOR_READ: u32 = 5;
const NORMAL_WRITE: u32 = 6;
const NORMAL_READ: u32 = 7;
const OPCODE_READ: u32 = 15;

impl w65c02s::System for System {
    fn read(&mut self, cpu: &mut W65C02S, addr: u16) -> u8 {
        self.perform_read(cpu, NORMAL_READ, addr)
    }
    fn read_locked(&mut self, cpu: &mut W65C02S, addr: u16) -> u8 {
        self.perform_read(cpu, LOCKED_READ, addr)
    }
    // 0x03 = fast NOP
    fn read_opcode(&mut self, cpu: &mut W65C02S, addr: u16) -> u8 {
        let ret = self.perform_read(cpu, OPCODE_READ, addr);
        if self.vector_has_been_pulled {
            if let Some(last_pc) = self.last_pc {
                if self.terminate_on_infinite_loop && addr == last_pc {
                    self.termination_cause = Some("infinite_loop");
                    return 0x03;
                }
            }
            self.last_pc = Some(addr);
            if self.terminate_on_zero_fetch && addr < 0x0100 {
                self.termination_cause = Some("zero_fetch");
                return 0x03;
            }
            if self.terminate_on_stack_fetch && addr >= 0x0100 && addr < 0x0200 {
                self.termination_cause = Some("stack_fetch");
                return 0x03;
            }
            if self.terminate_on_vector_fetch && addr >= 0xFFFA {
                self.termination_cause = Some("vector_fetch");
                return 0x03;
            }
            if self.terminate_on_brk && ret == w65c02s::op::BRK {
                self.termination_cause = Some("brk");
                return 0x03;
            }
        }
        ret
    }
    fn read_vector(&mut self, cpu: &mut W65C02S, addr: u16) -> u8 {
        self.vector_has_been_pulled = true;
        self.perform_read(cpu, VECTOR_READ, addr)
    }
    fn write(&mut self, cpu: &mut W65C02S, addr: u16, value: u8) {
        self.perform_write(cpu, NORMAL_WRITE, addr, value)
    }
    fn write_locked(&mut self, cpu: &mut W65C02S, addr: u16, value: u8) {
        self.perform_write(cpu, LOCKED_WRITE, addr, value)
    }
}

fn main() {
    let job = {
        let stdin = std::io::stdin();
        let mut stdin = stdin.lock();
        get_job(&mut stdin)
    }.unwrap();
    let mut system = System::new(&job);
    let mut cpu = W65C02S::new();
    while system.termination_cause.is_none() && system.num_cycles < system.cycles_to_run {
        cpu.step(&mut system);
    }
    if system.num_cycles >= system.cycles_to_run && system.termination_cause.is_none() {
        system.termination_cause = Some("limit");
    }
    let mut result = serde_json::Map::new();
    if let Some(last_pc) = system.last_pc {
        result.insert("last_pc".to_string(), Value::from(last_pc));
    }
    result.insert("num_cycles".to_string(), Value::from(system.num_cycles));
    if let Some(termination_cause) = system.termination_cause {
        result.insert("termination_cause".to_string(), Value::from(termination_cause));
    }
    if !system.cycles.is_empty() {
        result.insert("cycles".to_string(), Value::from(system.cycles));
    }
    match job.serial_out_fmt {
        None => (),
        Some(DataType::Utf8) => { result.insert("serial_out_data".to_string(), Value::from("utf8:".to_owned()+&String::from_utf8_lossy(&system.serial_out_data[..]))); },
        Some(DataType::Base64) => { result.insert("serial_out_data".to_string(), Value::from("base64:".to_owned()+&base64::encode(&system.serial_out_data[..]))); },
    }
    println!("{}", Value::Object(result).to_string());
}
