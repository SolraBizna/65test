The web API accepts jobs in UTF-8 encoded JSON form. A job must be less than or equal to 2,000,000 bytes in size.

Where binary data is required, give a string with one of two prefixes:

- `"base64:..."` = Binary data encoded in standard base64
- `"utf8:..."` = Text, to be encoded in UTF-8

The following data strings are equivalent:

- `"base64:VGhlIHVsdGltYXRlIENoaW5lc2UgY2hhcmFjdGVyOiDkupU="`
- `"utf8:The ultimate Chinese character: 井"`
- `"utf8:The ultimate Ch\u0069nese character: \u4E95"`

In all three cases, the encoded data is:

```
00000000  54 68 65 20 75 6c 74 69  6d 61 74 65 20 43 68 69  |The ultimate Chi|
00000010  6e 65 73 65 20 63 68 61  72 61 63 74 65 72 3a 20  |nese character: |
00000020  e4 ba 95                                          |...|
00000023
```

# Request

- `init`: An array of initialization objects. Must be present.
- `rwmap`: An array of inclusive memory ranges of the form `[first,last]`. Memory ranges may (uselessly) overlap. A maximum of 8 ranges may be specified. Default: `[[0,511]]` (the stack and zero page are writable)
- `serial_in_addr`: Address of the serial input. Default null (no serial input).
- `serial_out_addr`: Address of the serial output. Default null (no serial output). This address does not need to be marked writable by `rwmap`.
- `serial_in_data`: The data to provide on the serial port. No length limit, apart from the overall limit on job size.
- `serial_out_fmt`: "base64" for base64-encoded serial output, "utf8" for UTF8-encoded serial output (errors out if malformed), null (default) for discarding serial output. Only up to 131,072 bytes of output will be returned.
- `show_cycles`: If `true`, the response will contain a detailed description of every bus cycle. No more than 1,000 cycles will be shown.
- `max_cycles`: The job will terminate after running this many cycles (including the nine-cycle reset sequence). This cannot exceed 10,000,000 (which is also the default) or be less than 9.
- `terminate_on_brk`: The job will terminate if a BRK instruction is fetched. Default true.
- `terminate_on_infinite_loop`: The job will terminate if the same instruction is fetched twice in a row. Default true.
- `terminate_on_zero_fetch`: The job will terminate if an address in the range `$0000-$00FF` is fetched as an opcode. Default true.
- `terminate_on_stack_fetch`: The job will terminate if an address in the range `$0100-$01FF` is fetched as an opcode. Default true.
- `terminate_on_vector_fetch`: The job will terminate if an address in the range `$FFFA-$FFFF` is fetched as an opcode. Default true.
- `terminate_on_bad_write`: The job will terminate if a write is made to a read-only address.
- `nmi`: An array of cycle numbers at which the NMIB input will toggle. Default `[]` (no NMIs). Up to 20 are allowed.
- `irq`: An array of cycle numbers at which the IRQB input will toggle. Default `[]` (no IRQs). Up to 20 are allowed.
- `rdy`: An array of cycle numbers at which the RDY input will toggle. Default `[]` (always ready). Up to 20 are allowed.
- `so`: An array of cycle numbers at which the SO input will toggle. Default `[]` (no overflows set). Up to 20 are allowed. Starts to get weird if serial ports are in use.
- `res`: An array of cycle numbers at which the RES input will toggle. Default `[]` (no extra resets). Up to 20 are allowed. Note that there will always be a reset just before cycle 0, regardless of what you put here.

## Initialization

Before initialization records are applied, all memory is zeroed. Exception: For convenience, `$fffd` (the high byte of the reset vector) is initialized to 2; therefore, if you don't overwrite the reset vector, execution of your code will begin at `$0200`.

Each initialization object has the following keys:

- `base`: The start address of this block. Mandatory.
- `data`: The data to write to memory.
- `size`: The length of this block. `base+size` must not exceed 65536. Optional. If greater than the length of `data`, the data is **repeated**. If less than the length of `data`, the data is truncated.

# Serial ports

When reading the serial input address, one of two things will happen:

- If `serial_in_data` is not fully consumed, the next byte will be fetched.
- Otherwise, `$00` will be fetched and the SO pin will be asserted for one cycle.

When writing to the serial output address, one of two things will happen:

- If there is still room for serial output, the byte will be added.
- Otherwise, the SO pin will be asserted for one cycle.

If you `CLV` before doing serial IO, you can check the status of the V bit to determine if the end of the input/output has been reached.

# Response

If there was an error, the status code will be 4xx or 5xx. With a 4xx status, the response body will be a `text/plain` error message. With a 5xx status, the problem is purely server side and the error was logged there; the response is non-meaningful. Otherwise, the status code will be 200 and the response body will be an `application/json` response record.

Keys:

- `num_cycles`: Number of cycles that executed from the beginning of the reset sequence to the cycle on which the job terminated. Always returned.
- `last_pc`: The address of the last opcode fetch. If the job terminated due to one of the `terminate_on_*` cases, this is the address of the opcode that triggered termination.
- `termination_cause`: One of `"limit"`, `"brk"`, `"infinite_loop"`, `"zero_fetch"`, `"stack_fetch"`, `"vector_fetch"`, or `"bad_write"` depending on what caused the job to stop.
- `cycles`: An array of "cycle strings" giving the state of the bus at each cycle. Present only if `show_cycles` is true. Will not include any cycles before the reset vector pull.
- `serial_out_data`: The data that was outputted on the serial port, in the requested format. Present only if `serial_out_fmt` is not null.

## Cycle strings

Form: `"TAAAADD"`

- T: The type of bus cycle. (See below)
- AAAA: The address on the A bus before the falling edge of this cycle.
- DD: The data on the D bus after the falling edge of this cycle. Whether this came from the processor or your job depends on the type of bus cycle.

| T    | RWB  | VPB  | MLB  | SYNC | What                                     |
| ---- | ---- | ---- | ---- | ---- | ---------------------------------------- |
| 0    | low  | low  | low  | low  | (impossible)                             |
| 1    | HIGH | low  | low  | low  | (impossible)                             |
| 2    | low  | HIGH | low  | low  | Locked write (for read-modify-write)     |
| 3    | HIGH | HIGH | low  | low  | Locked read (for read-modify-write)      |
| 4    | low  | low  | HIGH | low  | (impossible)                             |
| 5    | HIGH | low  | HIGH | low  | Vector fetch                             |
| 6    | low  | HIGH | HIGH | low  | Normal write                             |
| 7    | HIGH | HIGH | HIGH | low  | Normal read                              |
| 8    | low  | low  | low  | HIGH | (impossible)                             |
| 9    | HIGH | low  | low  | HIGH | (impossible)                             |
| A    | low  | HIGH | low  | HIGH | (impossible)                             |
| B    | HIGH | HIGH | low  | HIGH | (impossible)                             |
| C    | low  | low  | HIGH | HIGH | (impossible)                             |
| D    | HIGH | low  | HIGH | HIGH | (impossible)                             |
| E    | low  | HIGH | HIGH | HIGH | (impossible)                             |
| F    | HIGH | HIGH | HIGH | HIGH | Opcode fetch                             |

Example sequence:

- `F025CEE`: Fetched opcode at `$025C`, got `$EE` (`INC $xxxx`)
- `7025D48`: Fetched byte at `$025D` (low byte of target address), got `$48` (`INC $xx48`)
- `7025E12`: Fetched byte at `$025E` (high byte of target address), got `$12` (`INC $1248`)
- `3124805`: Fetched byte at `$1248` as the read part of the read-modify-write. Got `$05`.
- `3124805`: Repeated the last cycle. (Memory read-modify-write incurs a spurious read.)
- `2124806`: Stored `$06` at `$1248` as the write part of the read-modify-write.

# The reset sequence

Here's a patched together description of the reset sequence.

- The RESB pin is asserted (brought LOW) for at least two clock cycles.
- While the RESB pin is asserted, the CPU performs a dummy read of the stack.
- The RESB pin is deasserted (brought HIGH).
- The CPU samples the RESB pin on the falling edge of PHI2. It detects the rising edge of RESB. It will initiate the reset sequence on the *next* falling edge of PHI2.
- Cycle 0: Dummy opcode fetch (SYNC) from a garbage address. In my observation, this fetch is typically from `$FFFF`.
- Cycle 1: Dummy operand fetch (non-SYNC) from a garbage address. For a warm reset, this fetch is typically from the vicinity of the old PC.
- Cycle 2, 3, 4: Dummy push; exactly like the PC/status push for an interrupt, but RWB is high (read) instead of low. (The stack pointer *is* updated.)
- Cycle 5, 6: Vector pull. Reset vector (`$FFFC`,`$FFFD`) is fetched.

I define cycle 0 as the first cycle of this reset sequence. When designing hardware to interface with the W65C02S, bear in mind that—depending on the timing of RESB's falling edge—there will be **either one or two** additional dummy cycles before the actual reset sequence begins.

For consistency between runs, the (nondeterministic) cycles before the reset vector pull are not reported in `cycles`.
