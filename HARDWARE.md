# Connections

These connection points may seem a bit arbitrary. Some of them are for mechanical convenience. In addition, A0-A15, RWB, VPB, MLB, and SYNC are all deliberately connected to pins corresponding to GPIO port C on the microcontroller, and D0-D7 and PHI2 to pins on port D. This is so that they can be manipulated very quickly. Other pins are infrequently accessed, so there's no big reason to optimize them that way. You can reassign those to whatever pins you like, as long as you change the definitions in `CPU.hh`.

Important: None of the other pins attached to port D are used by the Arduino Due...

Connections

| Arduino | W65C02S | SAM3X |
| ------- | ------- | ----- |
| 2       | RESB    |       |
| 3       | SOB     |       |
| 4       | BE      |       |
| 5       | NMIB    |       |
| 6       | IRQB    |       |
| 7       | SYNC    | PC23  |
| 8       | MLB     | PC22  |
| 9       | VPB     | PC21  |
| 10      | RDY     |       |
| 11      | D7      | PD7   |
| 12      | PHI2    | PD8   |
| 14      | D4      | PD4   |
| 15      | D5      | PD5   |
| 25      | D0      | PD0   |
| 26      | D1      | PD1   |
| 27      | D2      | PD2   |
| 28      | D3      | PD3   |
| 29      | D6      | PD6   |
| 33      | A0      | PC1   |
| 34      | A1      | PC2   |
| 35      | A2      | PC3   |
| 36      | A3      | PC4   |
| 37      | A4      | PC5   |
| 38      | A5      | PC6   |
| 39      | A6      | PC7   |
| 40      | A7      | PC8   |
| 41      | RWB     | PC9   |
| 44      | A15     | PC19  |
| 45      | A14     | PC18  |
| 46      | A13     | PC17  |
| 47      | A12     | PC16  |
| 48      | A11     | PC15  |
| 49      | A10     | PC14  |
| 50      | A9      | PC13  |
| 51      | A8      | PC12  |
