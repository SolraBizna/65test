use std::io;
use std::io::prelude::*;
use crc::{crc32,Hasher32};
use std::hash::Hasher;

#[derive(Debug)]
enum InState {
    GetRun,
    GetRunDeferredZero,
    Eof, Err,
    // value is number of nonzero values to read and return
    Run(u8), // run was 1-254
    LongRun(u8), // run was 255
}
pub struct In<'a, T: Iterator<Item=u8> + 'a> {
    it: &'a mut T,
    state: InState,
    crc: crc32::Digest,
}

impl<'a, T: Iterator<Item=u8> + 'a> In<'a, T> {
    pub fn new(it: &'a mut T) -> In<'a, T> {
        In { it, state: InState::GetRun, crc: crc32::Digest::new(crc32::IEEE) }
    }
    pub fn crc(&self) -> u32 {
        self.crc.sum32()
    }
}

impl<'a, T: Iterator<Item=u8> + 'a> Iterator for In<'a, T> {
    type Item = u8;
    fn next(&mut self) -> Option<u8> {
        loop {
            match self.state {
                InState::GetRun => {
                    match self.it.next() {
                        None => self.state = InState::Err,
                        Some(0) => self.state = InState::Eof,
                        Some(255) => self.state = InState::LongRun(254),
                        Some(len) => self.state = InState::Run(len),
                    }
                },
                InState::GetRunDeferredZero => {
                    match self.it.next() {
                        None => self.state = InState::Err,
                        Some(0) => self.state = InState::Eof,
                        Some(255) => {
                            self.state = InState::LongRun(254);
                            self.crc.write_u8(0);
                            return Some(0)
                        },
                        Some(len) => {
                            self.state = InState::Run(len);
                            self.crc.write_u8(0);
                            return Some(0)
                        },
                    }
                },
                InState::Eof | InState::Err => return None,
                InState::Run(rem) => {
                    if rem > 1 {
                        match self.it.next() {
                            None | Some(0) => {
                                self.state = InState::Err
                            },
                            Some(x) => {
                                self.state = InState::Run(rem-1);
                                self.crc.write_u8(x);
                                return Some(x)
                            }
                        }
                    }
                    else {
                        self.state = InState::GetRunDeferredZero
                    }
                },
                InState::LongRun(rem) => {
                    if rem > 1 {
                        match self.it.next() {
                            None | Some(0) => {
                                self.state = InState::Err
                            },
                            Some(x) => {
                                self.state = InState::Run(rem-1);
                                self.crc.write_u8(x);
                                return Some(x)
                            }
                        }
                    }
                    else {
                        self.state = InState::GetRun
                    }
                },
            }
        }
    }
}

impl<'a, T: Iterator<Item=u8> + 'a> Read for In<'a, T> {
    fn read(&mut self, buf: &mut [u8]) -> io::Result<usize> {
        let mut i = 0;
        while i < buf.len() {
            match self.next() {
                None => break,
                Some(x) => {
                    buf[i] = x;
                    i = i + 1;
                },
            }
        }
        Ok(i)
    }
}

pub struct Out<'a> {
    buf: &'a mut [u8],
    pos: usize,
    runlength: usize,
    crc: crc32::Digest,
}

impl<'a> Out<'a> {
    pub fn new(buf: &'a mut [u8]) -> Out<'a> {
        Out { buf, pos: 0, runlength: 0, crc: crc32::Digest::new(crc32::IEEE) }
    }
    pub fn finish(mut self) -> io::Result<usize> {
        let crc = self.crc.sum32();
        self.write_no_crc(&[(crc >> 24) as u8,
                            (crc >> 16) as u8,
                            (crc >> 8) as u8,
                            crc as u8,])?;
        if self.runlength > 0 {
            self.buf[self.pos] = (self.runlength + 1) as u8;
            self.pos += self.runlength + 1;
        }
        self.buf[self.pos] = 0;
        Ok(self.pos + 1)
    }
    fn write_no_crc(&mut self, buf: &[u8]) -> io::Result<usize> {
        for &c in buf {
            self.runlength += 1;
            if c == 0 {
                self.buf[self.pos] = self.runlength as u8;
                self.pos += self.runlength;
                self.runlength = 0;
            }
            else if self.runlength == 255 {
                self.buf[self.pos] = 255;
                self.pos += 255;
                self.runlength = 1;
                self.buf[self.pos+self.runlength] = c;
            }
            else {
                self.buf[self.pos+self.runlength] = c;
            }
        }
        Ok(buf.len())
    }
}

impl<'a> Write for Out<'a> {
    // if buf isn't big enough, we'll just panic on a bounds check, WOO
    fn write(&mut self, buf: &[u8]) -> io::Result<usize> {
        Hasher::write(&mut self.crc, buf);
        self.write_no_crc(buf)
    }
    fn flush(&mut self) -> io::Result<()> {
        Ok(())
    }
}
