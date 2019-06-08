This repository contains source code for all the tools related to my work on testing the [W65C02S](https://www.mouser.com/new/westerndesigncenter/wdc-w65c02s/), the most advanced direct descendent of the venerable 6502. It makes use of an [Arduino Due](https://store.arduino.cc/usa/arduino-due) connected to every significant pin of a DIP-44 W65C02S.

See [HARDWARE.md](HARDWARE.md) for information on how the W65C02S is connected to the Arduino, [CONTROL.md](CONTROL.md) for information on how the Arduino and the host PC communicate, and [API.md](API.md) for information on the API presented by the CGI script.

My build of the CGI script is currently hosted at <https://bunker.tejat.net/private/public/65test.cgi>, assuming I remember to leave the thing plugged in. Feel free to use it. Please don't use up too much of my bandwidth.

# arsprove

`arsprove` is a test suite containing about 4500 tests, and code to compare the core used in the [ARS Emulator](https://github.com/SolraBizna/ars-emu) (hence the name) and my Rust [`w65c02s` crate](https://crates.io/crates/w65c02s) against real hardware traces obtained with this CGI script. It could be trivially modified to test other simulators, with simpler adapters that consume input and produce output in the same format as the CGI script.

It's a fairly exhaustive test suite, but a few cases aren't covered. For instance, rapid reset/NMI pulses, use of RDY as an *input*, and the SOB pin are not covered. I consider these to be only minor problems.

`arsprove` requires GNU make, Lua 5.3, and a recent version of [WLA-DX](https://github.com/vhelin/wla-dx/).

# Licensing

This repository is licensed under version 3 of the GNU General Public License. See [here](LICENSE.md) for the full text of the license.
