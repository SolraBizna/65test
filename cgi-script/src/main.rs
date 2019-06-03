#![feature(try_blocks)]

extern crate outer_cgi;
extern crate base64;
extern crate serde;
#[macro_use]
extern crate serde_json;
#[macro_use]
extern crate serde_derive;
extern crate serial;
extern crate fs2;
extern crate crc;

mod comm;
mod cobs;

use std::collections::HashMap;
use std::io;
use std::io::{BufRead,Write};
use std::fmt;
use std::borrow::BorrowMut;
use std::ops::Deref;
use std::fs::File;
use std::path::{Path, PathBuf};
use serde::de;
use serde::de::Error as SerdeDeError;
use outer_cgi::IO;
use fs2::FileExt;
use comm::Comm;

const MAX_JOB_SIZE: usize = 2000000;
const MIN_CYCLE_COUNT: u32 = 9;
const MAX_CYCLE_COUNT: u32 = 10000000;
const MAX_SPECIAL_CYCLES: usize = 20;
const MAX_OVERALL_RETRIES: u32 = 3;
const CYCLES_TO_REPORT: usize = 1000;
const SERIAL_IN_BLOCK_SIZE: usize = 32;

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

fn failure(io: &mut IO, status: Option<&'static str>, body: &str)
           -> io::Result<i32> {
    if let Some(status) = status {
        io.write_all(format!("Status: {}\n", status).as_bytes())?;
    }
    io.write_all(format!("Content-type: text/plain; charset=utf-8\n\
                          Content-length: {}\n\
                          \n", body.len()).as_bytes())?;
    io.write_all(body.as_bytes())?;
    return Ok(0)
}

fn get_job(io: &mut IO) -> Result<Job, serde_json::Error> {
    try {
        let mut job: Job = serde_json::from_reader(io.borrow_mut())?;
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
            }
        }
        job
    }
}

fn get_port_lock() -> io::Result<(File, PathBuf)> {
    let mut file = File::open(".65test_serial_path.txt")?;
    file.lock_exclusive()?;
    // Ick!
    let port_path = {
        let mut bufread = io::BufReader::new(&mut file);
        let mut line = String::new();
        bufread.read_line(&mut line)?;
        while line.ends_with("\n") || line.ends_with("\r") {
            let nulen = line.len()-1;
            line.truncate(nulen);
        }
        line
    };
    return Ok((file, port_path.into()));
}

fn send_one_init(mut data: &[u8], comm: &mut Comm)
                 -> io::Result<()> {
    while data.len() > comm::MAX_PHYSICAL_PACKET_SIZE {
        comm.send_packet(0x01, &data[..comm::MAX_PHYSICAL_PACKET_SIZE],
                         false)?;
        data = &data[comm::MAX_PHYSICAL_PACKET_SIZE..];
    }
    comm.send_packet(0x01, data, false)?;
    Ok(())
}

fn send_init(base: u16, mut rem: usize, data: &[u8],
             comm: &mut Comm) -> io::Result<()> {
    comm.send_packet(0x09, &[(base >> 8) as u8, base as u8], false)?;
    while rem > 0 {
        let sublen = rem.min(data.len());
        send_one_init(&data[..sublen], comm)?;
        rem -= sublen;
    }
    Ok(())
}

fn attempt_job(io: &mut IO, job: &Job, port_path: &Path)
               -> io::Result<()> {
    let mut comm = Comm::new(port_path)?;
    let mut buf = Vec::with_capacity(comm::MAX_LOGICAL_PACKET_SIZE);
    // Send initialization records
    for rec in job.init.iter() {
        let size = rec.size.map(|x| x as usize)
            .unwrap_or(rec.data.len());
        if rec.data.len() < size
        && rec.data.len() < comm::MAX_LOGICAL_PACKET_SIZE {
            buf.clear();
            while buf.len() < comm::MAX_LOGICAL_PACKET_SIZE - rec.data.len()
            && buf.len() < size {
                buf.extend_from_slice(&rec.data);
            }
            send_init(rec.base, size, &buf, &mut comm)?;
        }
        else {
            send_init(rec.base, size, &rec.data, &mut comm)?;
        }
    }
    // Send RW map
    if let Some(rw) = &job.rwmap {
        buf.clear();
        for rw in rw {
            buf.write_all(&[(rw.start >> 8) as u8,
                            rw.start as u8,
                            (rw.end >> 8) as u8,
                            rw.end as u8])?;
        }
        comm.send_packet(0x02, &buf[..],
                         false)?;
    }
    // Serial in
    if let Some(addr) = job.serial_in_addr {
        comm.send_packet(0x03, &[(addr >> 8) as u8,
                                 addr as u8], false)?;
    }
    // Serial out
    if let Some(addr) = job.serial_out_addr {
        comm.send_packet(0x04, &[(addr >> 8) as u8,
                                 addr as u8], false)?;
    }
    // Show cycles
    if let Some(true) = job.show_cycles {
        comm.send_packet(0x05, &[(CYCLES_TO_REPORT >> 24) as u8,
                                 (CYCLES_TO_REPORT >> 16) as u8,
                                 (CYCLES_TO_REPORT >> 8) as u8,
                                 CYCLES_TO_REPORT as u8], false)?;
    }
    // Max cycles
    if let Some(max) = job.max_cycles {
        comm.send_packet(0x06, &[(max >> 24) as u8,
                                 (max >> 16) as u8,
                                 (max >> 8) as u8,
                                 max as u8], false)?;
    }
    // Termination causes
    let mut termination_flag = 0x3F;
    if let Some(false) = job.terminate_on_brk {
        termination_flag &= !0x01;
    }
    if let Some(false) = job.terminate_on_infinite_loop {
        termination_flag &= !0x02;
    }
    if let Some(false) = job.terminate_on_zero_fetch {
        termination_flag &= !0x04;
    }
    if let Some(false) = job.terminate_on_stack_fetch {
        termination_flag &= !0x08;
    }
    if let Some(false) = job.terminate_on_vector_fetch {
        termination_flag &= !0x10;
    }
    if let Some(false) = job.terminate_on_bad_write {
        termination_flag &= !0x20;
    }
    if termination_flag != 0x3F {
        comm.send_packet(0x07, &[termination_flag], false)?;
    }
    // Flag changes
    #[derive(Debug)]
    enum Flag {Nmi, Irq, Rdy, So, Res};
    let job_changes = [
        (Flag::Nmi, &job.nmi),
        (Flag::Irq, &job.irq),
        (Flag::Rdy, &job.rdy),
        (Flag::So, &job.so),
        (Flag::Res, &job.res),
    ];
    let mut changes = Vec::new();
    for (typ, opt) in job_changes.iter() {
        if let Some(v) = opt {
            for e in v.iter() {
                changes.push((typ, *e))
            }
        }
    }
    if !changes.is_empty() {
        changes.sort_by(|a,b| { a.1.cmp(&b.1) });
        let mut nmi_on = false;
        let mut irq_on = false;
        let mut rdy_on = true;
        let mut so_on = false;
        let mut res_on = false;
        buf.clear();
        for (typ, cycle) in changes {
            let (flag, id) = match typ {
                Flag::Nmi => (&mut nmi_on, 2),
                Flag::Irq => (&mut irq_on, 3),
                Flag::Rdy => (&mut rdy_on, 4),
                Flag::So => (&mut so_on, 1),
                Flag::Res => (&mut res_on, 0),
            };
            *flag = !*flag;
            if *flag {
                buf.push(0x80 | id);
            }
            else {
                buf.push(id);
            }
            buf.push((cycle >> 16) as u8);
            buf.push((cycle >> 8) as u8);
            buf.push(cycle as u8);
        }
        comm.send_packet(0x08, &buf[..], false)?;
    }
    comm.send_packet(0xFE, &[], true)?;
    let mut cycle_reports = Vec::new();
    let mut serial_out_data = Vec::new();
    let num_cycles;
    let execution_time;
    let last_pc;
    let termination_cause;
    let mut serial_in_data = match job.serial_in_data {
        Some(ref data) => &data[..],
        None => &[]
    };
    loop {
        let packet_type = comm.read_packet(&mut buf);
        match packet_type {
            Ok(0x01) => {
                // Cycle reports
                if buf.len() % 4 != 0 {
                    return Err(io::Error::new(io::ErrorKind::Other,
                                              "bad cycle report packet \
                                               length"))
                }
                for chunk in buf.chunks(4) {
                    cycle_reports.push(format!("{:07X}",
                                               ((chunk[0] as u32) << 24)
                                               | ((chunk[1] as u32) << 16)
                                               | ((chunk[2] as u32) << 8)
                                               | (chunk[3] as u32)));
                }
                if cycle_reports.len() > CYCLES_TO_REPORT {
                    return Err(io::Error::new(io::ErrorKind::Other,
                                              "too many cycle reports"))
                }
                comm.ack_packet(false)?
            },
            Ok(0x02) => {
                // Serial read request
                if buf.len() != 0 {
                    return Err(io::Error::new(io::ErrorKind::Other,
                                              "non-empty serial read \
                                               request"))
                }
                comm.ack_packet(true)?;
                let to_send
                    = SERIAL_IN_BLOCK_SIZE.min(serial_in_data.len());
                comm.send_packet(0x53, &serial_in_data[..to_send], true)?;
                serial_in_data = &serial_in_data[to_send..];
            },
            Ok(0x03) => {
                // Serial write
                if buf.len() == 0 {
                    return Err(io::Error::new(io::ErrorKind::Other,
                                              "empty serial write"))
                }
                if job.serial_out_fmt.is_some() {
                    serial_out_data.extend_from_slice(&buf[..]);
                }
                comm.ack_packet(false)?;
            },
            Ok(0x04) => {
                // Termination
                if buf.len() != 11 {
                    return Err(io::Error::new(io::ErrorKind::Other,
                                              "wrong termination length"))
                }
                num_cycles = ((buf[0] as u32) << 24)
                    | ((buf[1] as u32) << 16)
                    | ((buf[2] as u32) << 8)
                    | (buf[3] as u32);
                execution_time = ((buf[4] as u32) << 24)
                    | ((buf[5] as u32) << 16)
                    | ((buf[6] as u32) << 8)
                    | (buf[7] as u32);
                last_pc = ((buf[8] as u16) << 8)
                    | (buf[9] as u16);
                termination_cause = buf[10];
                comm.ack_packet(false)?;
                break;
            },
            Ok(x) => {
                return Err(io::Error::new(io::ErrorKind::Other,
                                          format!("unknown packet type \
                                                   {:02X}", x)))
            },
            Err(e) => {
                if false {
                    while let Some(x) = comm.next() {
                        if x != 0 {
                            eprintln!("{}", format!("related byte? {:02X}",
                                                    x));
                        }
                    }
                }
                return Err(e)
            }
        }
    }
    std::mem::drop(comm);
    let termination_cause = [
        "limit", "brk", "infinite_loop", "zero_fetch", "stack_fetch",
        "vector_fetch", "bad_write"
    ][termination_cause as usize];
    eprint!("{}",
            format!("job ran {} cycles in {}ms (about {}Hz), terminated by {}\
                     \n",
                    num_cycles, execution_time,
                    num_cycles * 1000 / execution_time.max(1),
                    termination_cause));
    let serial_out_data = match job.serial_out_fmt {
        None => None,
        Some(DataType::Utf8) =>
            Some("utf8:".to_owned()+&String::from_utf8_lossy(&serial_out_data[..])),
        Some(DataType::Base64) =>
            Some("base64:".to_owned()+&base64::encode(&serial_out_data[..])),
    };
    let reply = json!({
        "num_cycles":num_cycles,
        "last_pc":last_pc,
        "termination_cause":termination_cause,
        "serial_out_data":serial_out_data,
        "cycles":cycle_reports,
    });
    let reply = reply.to_string();
    io.write_all(format!("Content-type: application/json; charset=utf-8\n\
                          Content-length: {}\n\
                          \n", reply.len()).as_bytes())?;
    io.write_all(reply.as_bytes())?;
    Ok(())
}

fn handler(io: &mut IO, env: HashMap<String, String>) -> io::Result<i32> {
    match env.get("REQUEST_METHOD").map(String::as_str) {
        Some("POST") => (),
        _ => {
            io.write_all(b"Allow: POST\n")?;
            return failure(io,
                           Some("405 Method Not Allowed"),
                           "Only POST requests are allowed.")
        }
    }
    let content_length: usize = match env.get("CONTENT_LENGTH") {
        Some(value) => {
            match value.parse() {
                Ok(x) => x,
                Err(_) => {
                    eprintln!("Received an invalid CONTENT_LENGTH!");
                    return failure(io,
                                   Some("500 Internal Server Error"),
                                   "")
                }
            }
        },
        _ => {
            return failure(io,
                           Some("411 Length Required"),
                           "Your request must include a Content-Length \
                            header.")
        },
    };
    if content_length > MAX_JOB_SIZE {
        return failure(io,
                       Some("413 Request Entity Too Large"),
                       "Your request must not exceed 2,000,000 bytes in size.")
    }
    let job = match get_job(io.borrow_mut()) {
        Ok(job) => job,
        Err(e) => return failure(io.borrow_mut(),
                                 Some("400 Bad Request"),
                                 &format!("Error parsing your request:\n\n\
                                           {}", e))
    };
    let (_lock, port_path) = get_port_lock()?;
    for n in 0 .. MAX_OVERALL_RETRIES {
        match attempt_job(io.borrow_mut(), &job, &port_path) {
            Ok(_) => return Ok(0),
            Err(e) => {
                eprint!("{}",
                        format!("retry {} of {}: {}\n", n+1,
                                MAX_OVERALL_RETRIES, e));
                std::thread::sleep(std::time::Duration::from_millis(1000));
            }
        }
    }
    io.write_all(b"Status: 500 Internal Server Error\n")?;
    Ok(0)
}

fn main() {
    outer_cgi::main(|_|{}, handler)
}
