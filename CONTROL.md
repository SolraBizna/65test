This document describes the control protocol used to communicate with the 65test-programmed Arduino Due.

The protocol is packet based, but only in one direction at a time. The side sending packets is the Sender, and the other side is the Receiver. At reset, the host is the Sender. The host and device swap roles at protocol-dependent times.

There is an additional asymmetry where it comes to error handling. If the host encounters an error, it will simply reset the device and start over. If the device encounters an error, it will start outputting the "death sequence"; continuous `0x00`s. (This will lead the host to detect an error and initiate a reset-and-retry.)

Numbers are big-endian, where applicable.

# Physical

The lowest layer is RS-232, exposed over the Programming Port of the Due. 115200 baud, eight data bits, one stop bit, no parity, no flow control.

# Sender

The Sender sends packets and receives acks.

## Encoding

Next layer up is [COBS](http://conferences.sigcomm.org/sigcomm/1997/papers/p062.pdf). `0x00` signals the end of a packet. (Physical packets are not allowed to get long enough to require logic to break up very long strings of non-zero bytes, so that logic can be left out.)

## Framing

A packet's structure:

```c
struct packet {
  uint8_t type;
  uint8_t length;
  uint8_t data[length];
  uint8_t crc32[4];
};
```

- Serial input buffer: 128 bytes.
- Minus COBS overhead: 127 bytes.
- Minus trailing 0: 126 bytes.
- Minus type, length, and CRC bytes: 120 bytes.

The longest physical packet that can be transmitted therefore has a length of 120 bytes. The special packet type (zero) is used to transmit longer logical packets. Each 120-byte fragment up to the last one is transmitted as a type zero packet. The last packet of the fragment carries the true packet type. An arbitrary length limit of 1200 bytes is placed on logical packets; the Due's physical memory is extremely limited.

The CRC32 includes the type and length bytes. It is applied to physical packets, and is not influenced by any fragmentation that may have occurred on the logical level.

- `0x00`: If length is zero, this is a Keepalive and **must not trigger an acknowledgement**. If length is 120, this is a fragment. Otherwise, there has been an error.
- `0xFF`: Length MUST be zero. This is an Echo Request. The receiver is expected to send an Echo Response as soon as possible.

Except as described above, the meaning of packet types is defined on another layer.

# Receiver

The receiver receives packets and sends ACKs.

An ACK sequence is two `0x00` bytes followed by a non-zero type byte. This sequence can never be outputted from the sending side; a Sender and a Receiver are thus easily distinguished.

Type bytes:

1. The Receiver received and handled a logical packet.
2. The Receiver received a fragment.
3. The Receiver received and handled a logical packet, and believes that a role reversal should now occur. If the Sender doesn't agree, an error has occurred.
4. Wakeup part 1. (Device only)
5. Wakeup part 2. (Device only)
6. Wakeup part 3. (Device only)
7. Heartbeat. The Sender should send a Keepalive (or any other data) if it wants the receiver to keep paying attention.
8. Echo Response. The Receiver received an Echo Request.

# Waking up

When the device first emerges from reset, it will send the following wakeup sequence: 0 0 4 0 0 5 0 0 6. This allows the host to detect that the device has successfully reset, and that any leftover OS-buffered data from previous "lives" has been skipped. (Note that some leftover *wakeups* from previous, spurious resets may have been buffered; the host should take steps to ensure it has read all available wakeups before proceeding.)

On startup, *after* sending a wakeup sequence, if the device detects an unexpected bus state while resetting the CPU it will send a special "bus error sequence":

- `00 00 FF 00 FF 00 FF`: Bus error sequence header
- (3 bytes): A bitmask of significant bits on the bus
- (3 bytes): The bus state we expected
- (3 bytes): The bus state we actually observed
- (1 byte): The cycle number within the reset sequence
- (1 byte): 0 if the measurement was taken while PHI2 was low, 1 if PHI2 was high
- `DE`: Trailer

The bus state is given in the same format as a cycle report, but omitting the data byte.

# Timing out

## Host

When the host is looking for a wakeup sequence, it can have a relatively short timeout of just a second or so. If it doesn't see a wakeup during this time, it simply resets and hopes to try again.

Afterward, it should have a hard timeout of at least 10 seconds. Preferably, it should send an Echo Request or Heartbeat if it hasn't received data in, say, 5 seconds. (Naturally, this need apply only in Receiver mode.)

## Device

If the device hasn't seen any data whatsoever from the host in *at least* 30 seconds, it should send an Echo Request or Heartbeat (assuming it's not in the middle of reading something). If it hasn't received data in *at least* 60 seconds, it will send a long string of zeroes, then power off.

# Starting state

The host is the Sender and the device is the Receiver.

Packet types:

- `0x01`: Write to SRAM. Wraps.  
```c
uint8_t data[...];
```
- `0x02`: Establish the writeable memory ranges. Length must be a multiple of 4. A maximum of 8 ranges may be given.  
```c
uint16_t start;
uint16_t stop;
...
```
- `0x03`: Establish a serial input address.  
```c
uint16_t addr;
```
- `0x04`: Establish a serial output address.  
```c
uint16_t addr;
```
- `0x05`: Report a given number of cycles.  
```c
uint32_t max_cycles_to_report;
```
- `0x06`: Terminate after a given number of cycles.  
```c
uint32_t max_cycles;
```
- `0x07`: Termination flags. (One byte)  
```c
0x01 = BRK
0x02 = infinite loop
0x04 = zero fetch
0x08 = stack fetch
0x10 = vector fetch
0x20 = bad write
0xC0 = invalid! error!
```
- `0x08`: Up to 120 flag changes. Length must be exact. Cycle numbers should be in ascending order. (See source for definitions)  
```c
uint1_t new_state; // logical state, not physical state
uint7_t change;
uint24_t cycle_delay;
```
- `0x09`: Change the position at which the next `0x01` record will write.
- `0xFE`: Go! (Advances to Running state)

# Running state

The device is the Sender and the host is the Receiver.

Packet types:

- `0x01`: Batch of cycle reports. Defined the same as the "cycle lines" in the API, but with the high 4 bits always equal to 0.  
```c
uint32_t spec[...];
```
- `0x02`: Serial read request. Always empty. Switches to Serial Read state.
- `0x03`: Batch of serial output.  
```c
uint8_t outdata[...];
```
- `0x04`: Termination. After this message is acknowledged, the Arduino will shut down.  
```c
uint32_t num_cycles;
uint32_t num_milliseconds;
uint16_t last_pc;
uint8_t termination_cause;
```  
Values for termination_cause:
    - `0x00`: Ran out of cycles.
    - `0x01`: BRK
    - `0x02`: Infinite loop
    - `0x03`: Zero-page instruction fetch
    - `0x04`: Stack-page instruction fetch
    - `0x05`: Vector instruction fetch
    - `0x06`: bad write

# Serial Read state

The host is the Sender and the device is the Receiver.

The host will send exactly one `0x53` packet containing no more than 32 bytes of serial input data. The state is then returned to the Running state.
