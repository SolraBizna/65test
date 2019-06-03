use serial;
use serial::prelude::*;
use std;
use std::io;
use std::io::{BufRead, Read, Write};
use std::path::Path;
use std::time::Duration;
use std::borrow::BorrowMut;
use super::cobs;

pub const BUF_SIZE: usize = 128;
pub const MAX_PHYSICAL_PACKET_SIZE: usize = 120;
pub const MAX_LOGICAL_PACKET_SIZE: usize = MAX_PHYSICAL_PACKET_SIZE * 10;

#[derive(Debug)]
enum Mode {
    Raw, Sender, Receiver, ReceiverNeedAck
}

pub struct Comm {
    port: serial::SystemPort,
    raw_len: u32,
    raw_consumed: u32,
    raw_buf: [u8; BUF_SIZE],
    mode: Mode,
    sent_ping: bool,
}

fn eof() -> io::Error {
    io::Error::new(io::ErrorKind::UnexpectedEof,
                   "unexpected EOF")
}

impl Comm {
    pub fn new(path: &Path) -> io::Result<Comm> {
        let mut port = match serial::open(path) {
            Ok(port) => port,
            Err(_) =>
                return Err(io::Error::new(io::ErrorKind::Other,
                                          format!("Unable to open serial port \
                                                   {:?}", path.to_str())))
        };
        port.reconfigure(&|settings: &mut SerialPortSettings| {
            settings.set_baud_rate(serial::Baud115200)?;
            settings.set_char_size(serial::Bits8);
            settings.set_parity(serial::ParityNone);
            settings.set_stop_bits(serial::Stop1);
            settings.set_flow_control(serial::FlowNone);
            Ok(())
        })?;
        port.set_timeout(Duration::from_secs(1))?;
        let mut ret = Comm {
            port,
            raw_buf: unsafe { std::mem::uninitialized() },
            raw_len: 0,
            raw_consumed: 0,
            mode: Mode::Raw,
            sent_ping: false,
        };
        let mut valid_handshake = false;
        // Consume all input if we can, and try to get a wakeup sequence
        // This actually looks for the sequence [_, 4, 0, 0, 5, 0, 0, 6]...
        // which is close enough for our purposes.
        'outer: loop {
            if valid_handshake {
                ret.set_port_timeout(Duration::from_millis(10))?;
            }
            else {
                ret.set_port_timeout(Duration::from_secs(1))?;
            }
            match ret.next() {
                None => break,
                Some(255) => {
                    valid_handshake = false;
                    const WAKEUP_TRAILER: [u8; 4] = [0, 0xFF, 0, 0xFF];
                    for tsugi in WAKEUP_TRAILER.iter() {
                        match ret.next() {
                            None => break 'outer,
                            Some(c) => {
                                if c != *tsugi { break }
                            }
                        }
                    }
                    let mut report: [u8; 11] =
                        unsafe{std::mem::uninitialized()};
                    for n in 0 .. report.len() {
                        report[n] = match ret.next() {
                            None => break 'outer,
                            Some(c) => c,
                        };
                    }
                    // if a bus error report is in progress and a shutdown
                    // occurs before the error can be completed, we may miss
                    // a subsequent wakeup sequence...
                    match ret.next() {
                        None => break 'outer,
                        Some(0xDE) => {
                            let mask = ((report[0] as u32) << 16)
                                | ((report[1] as u32) << 8)
                                | (report[2] as u32);
                            let want = ((report[3] as u32) << 16)
                                | ((report[4] as u32) << 8)
                                | (report[5] as u32);
                            let got = ((report[6] as u32) << 16)
                                | ((report[7] as u32) << 8)
                                | (report[8] as u32);
                            let cycle = report[9];
                            let edge = match report[10] {
                                0 => "low",
                                1 => "high",
                                _ => continue // not a valid report
                            };
                            // explicitly format the whole thing so that we
                            // output the message all at once (so Apache is
                            // less likely to mangle the message)
                            eprint!("{}",
                                    format!("Looks like an unexpected bus \
                                             state was seen during the \
                                             reset sequence. Check the \
                                             connections! (cycle={}, \
                                             edge={}, mask={:05X}, \
                                             want={:05X}, got={:05X})\n",
                                            cycle, edge, mask, want, got));
                        },
                        _ => (),
                    }
                },
                Some(4) => {
                    valid_handshake = false;
                    const WAKEUP_TRAILER: [u8; 6] = [0, 0, 5, 0, 0, 6];
                    for tsugi in WAKEUP_TRAILER.iter() {
                        match ret.next() {
                            None => break 'outer,
                            Some(c) => {
                                if c != *tsugi { break }
                            }
                        }
                    }
                    valid_handshake = true;
                },
                Some(_) => {
                    valid_handshake = false;
                },
            }
        }
        if valid_handshake {
            ret.set_port_timeout(Duration::from_secs(5))?;
            ret.enter_mode(Mode::Sender)?;
            Ok(ret)
        }
        else {
            Err(io::Error::new(io::ErrorKind::Other,
                               "Did not see a wakeup sequence"))
        }
    }
    fn enter_mode(&mut self, mode: Mode) -> io::Result<()> {
        let valid = match self.mode {
            Mode::Raw => {
                match mode {
                    Mode::Sender => true,
                    _ => false,
                }
            },
            Mode::Receiver => {
                match mode {
                    Mode::ReceiverNeedAck => true,
                    _ => false,
                }
            },
            Mode::ReceiverNeedAck => {
                match mode {
                    Mode::Receiver | Mode::Sender => true,
                    _ => false,
                }
            },
            Mode::Sender => {
                match mode {
                    Mode::Receiver => true,
                    _ => false,
                }
            },
        };
        if !valid {
            panic!("invalid mode transition {:?} -> {:?}", self.mode, mode)
        }
        else {
            self.mode = mode;
            Ok(())
        }
    }
    pub fn read_packet(&mut self, buf: &mut Vec<u8>) -> io::Result<u8> {
        match self.mode {
            Mode::Receiver => (),
            _ => panic!("read_packet called from wrong mode ({:?})", self.mode)
        }
        buf.clear();
        loop {
            let packet_type;
            let length;
            {
                let mut i = cobs::In::new(self.borrow_mut());
                packet_type = i.next().ok_or_else(eof)?;
                length = i.next().ok_or_else(eof)?;
                match packet_type {
                    0 => {
                        if length != 0
                        && length != MAX_PHYSICAL_PACKET_SIZE as u8 {
                            return Err(io::Error::new(io::ErrorKind::Other,
                                                      "Invalid fragment \
                                                       length"))
                        }
                    },
                    255 => {
                        if length != 0 {
                            return Err(io::Error::new(io::ErrorKind::Other,
                                                      "Invalid echo request \
                                                       length"))
                        }
                    },
                    _ => {
                        if length > MAX_PHYSICAL_PACKET_SIZE as u8 {
                            return Err(io::Error::new(io::ErrorKind::Other,
                                                      "Physical packet too \
                                                       large"))
                        }
                    },
                }
                if length != 0 {
                    buf.reserve(length as usize);
                    let beg = buf.len();
                    unsafe { buf.set_len(beg + length as usize) };
                    i.read_exact(&mut buf[beg..])?;
                }
                let crc_calc = i.crc();
                let mut crc_rx: [u8; 4] = unsafe {std::mem::uninitialized()};
                i.read_exact(&mut crc_rx[..])?;
                match i.next() {
                    None => (),
                    Some(_) => 
                        return Err(io::Error::new(io::ErrorKind::Other,
                                                  "Framing error"))
                }
                let crc_rx =
                    ((crc_rx[0] as u32) << 24)
                    | ((crc_rx[1] as u32) << 16)
                    | ((crc_rx[2] as u32) << 8)
                    | (crc_rx[3] as u32);
                if crc_rx != crc_calc {
                    return Err(io::Error::new(io::ErrorKind::Other,
                                             format!("CRC error (read {:08X}, \
                                                      calculated {:08X})",
                                                     crc_rx, crc_calc)))
                }
                if let Some(_) = i.next() {
                    return Err(io::Error::new(io::ErrorKind::Other,
                                              "Framing error"))
                }
            }
            match packet_type {
                0 if length == 0 => {
                    // Keepalive. Do not acknowledge.
                    self.sent_ping = false;
                },
                0 => {
                    // Fragment. Send a frag ack.
                    self.port.write_all(&[0,0,2])?
                },
                255 => {
                    // Echo request. Send an echo response.
                    self.port.write_all(&[0,0,8])?
                },
                x => {
                    self.enter_mode(Mode::ReceiverNeedAck)?;
                    return Ok(x)
                }
            }
        }
    }
    pub fn ack_packet(&mut self, flip: bool) -> io::Result<()> {
        match self.mode {
            Mode::ReceiverNeedAck => (),
            _ => panic!("ack_packet called from wrong mode ({:?})", self.mode)
        }
        if flip {
            self.port.write_all(&[0,0,3])?;
            self.enter_mode(Mode::Sender)
        }
        else {
            self.port.write_all(&[0,0,1])?;
            self.enter_mode(Mode::Receiver)
        }
    }
    pub fn send_packet(&mut self, typ: u8, mut data: &[u8], should_flip: bool)
                       -> io::Result<()> {
        match self.mode {
            Mode::Sender => (),
            _ => panic!("send_packet called from wrong mode ({:?})", self.mode)
        }
        while data.len() > MAX_PHYSICAL_PACKET_SIZE {
            let len = {
                let mut o = cobs::Out::new(&mut self.raw_buf[..]);
                o.write_all(&[0, MAX_PHYSICAL_PACKET_SIZE as u8])?;
                o.write_all(&data[..MAX_PHYSICAL_PACKET_SIZE])?;
                o.finish()?
            };
            self.port.write_all(&self.raw_buf[..len])?;
            match self.next() { Some(0) => (), _ => return Err(eof()) }
            match self.next() { Some(0) => (), _ => return Err(eof()) }
            match self.next() { Some(2) => (), _ => return Err(eof()) }
            data = &data[MAX_PHYSICAL_PACKET_SIZE..];
        }
        let len = {
            let mut o = cobs::Out::new(&mut self.raw_buf[..]);
            o.write_all(&[typ, data.len() as u8])?;
            o.write_all(data)?;
            o.finish()?
        };
        self.port.write_all(&self.raw_buf[..len])?;
        match self.next() { Some(0) => (), _ => {while let Some(_) = self.next() {} return Err(eof()) }}
        match self.next() { Some(0) => (), _ => return Err(eof()) }
        loop {
            let did_flip = match self.next() {
                Some(1) => false,
                Some(3) => true,
                Some(8) if self.sent_ping => {
                    self.sent_ping = false;
                    continue;
                },
                Some(0) =>
                    return Err(io::Error::new(io::ErrorKind::InvalidData,
                                              "unexpected shutdown")),
                Some(_) =>
                    return Err(io::Error::new(io::ErrorKind::InvalidData,
                                              "protocol error")),
                None => return Err(eof())
            };
            assert_eq!(did_flip, should_flip);
            if did_flip {
                self.enter_mode(Mode::Receiver)?;
            }
            break;
        }
        Ok(())
    }
    fn set_port_timeout(&mut self, duration: Duration) -> serial::Result<()> {
        self.port.set_timeout(duration)
    }
    fn get_byte(&mut self) -> Option<u8> {
        match self.fill_buf() {
            Ok(r) if r.is_empty() => return None,
            Err(_) => return None,
            _ => (),
        }
        let ret = self.raw_buf[self.raw_consumed as usize];
        self.consume(1);
        Some(ret)
    }
}

impl Read for Comm {
    fn read(&mut self, buf: &mut [u8]) -> io::Result<usize> {
        self.port.read(buf)
    }
}

impl BufRead for Comm {
    fn fill_buf(&mut self) -> io::Result<&[u8]> {
        if self.raw_consumed >= self.raw_len {
            let len = self.port.read(&mut self.raw_buf[..])?;
            self.raw_consumed = 0;
            self.raw_len = len as u32;
        }
        Ok(&self.raw_buf[self.raw_consumed as usize
                         .. self.raw_len as usize])
    }
    fn consume(&mut self, amt: usize) {
        assert!(self.raw_consumed as usize + amt <= self.raw_len as usize);
        self.raw_consumed += amt as u32
    }
}

impl Iterator for Comm {
    type Item = u8;
    fn next(&mut self) -> Option<u8> {
        match self.get_byte() {
            Some(x) => Some(x),
            None if !self.sent_ping => {
                match self.mode {
                    Mode::Receiver => {
                        self.port.write_all(&[0,0,7]).is_ok();
                    },
                    Mode::Sender => {
                        self.port.write_all(&[0x02,0xFF,0x05,0xD2,0xFD,0xEF,
                                              0x8D,0x00]).is_ok();
                    },
                    _ => return None,
                }
                self.sent_ping = true;
                self.get_byte()
            },
            None => None,
        }
    }
}

impl Drop for Comm {
    fn drop(&mut self) {
        self.port.write_all(&[0,0,0,0]).is_ok();
    }
}
