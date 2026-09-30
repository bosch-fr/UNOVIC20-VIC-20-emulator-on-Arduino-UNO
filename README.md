# UNOVIC20 — VIC-20 emulator on Arduino UNO

A working Commodore VIC-20 emulator running on a single Arduino Uno.

Composite video, PS/2 keyboard, 9 save slots, BASIC sound. No PC, no FPGA.

<img width="600" height="450" alt="1" src="https://github.com/user-attachments/assets/6dbb8769-82cd-40ad-addf-3426bac8fcdb" />

# Features

- Real VIC-20 KERNAL and BASIC ROM
- Composite PAL video, 22×23 characters
- PS/2 keyboard (US layout)
- 9 save/load slots on external I²C EEPROM
- BASIC sound via `POKE` to VIC sound registers
- 6653 bytes free for BASIC

<img width="600" height="504" alt="3" src="https://github.com/user-attachments/assets/d45873b5-294b-45f4-912b-75097261c8cc" />

# What works

Boots to `READY.`, runs BASIC programs, saves and loads state, plays simple
sound effects.
Display is very stable on standard monitor.

# What doesn't

- Blinking cursor — a static `>` marker is drawn instead
- Cartridge / tape loading** — not supported.
- Keyboard glitch
 — typing briefly shifts the screen horizontally.

# Hardware

| Part | Purpose |
|------|---------|
| Arduino Uno | Main CPU + video |
| 23LC1024 SPI SRAM (128 KB) | Emulated 6502 RAM |
| 24LC512 I²C EEPROM (64 KB) | Save slots |
| PS/2 female connector | Keyboard |
| Passive piezo buzzer | Sound |
| 2× push buttons | Save / Load |
| 3× 4.7 kΩ, 1× 1 kΩ, 1× 680 Ω | Pull-ups + video + buzzer |

Total: under $15.

# Wiring

| Pin | Function |
|-----|----------|
| D2 | Composite video — sync |
| D3 | Composite video — blanking |
| D4 | MSPI clock (pixel clock) |
| D6 | Save button |
| D7 | Load button |
| D8 | PS/2 CLK |
| D9 | PS/2 DATA |
| D10 | SRAM chip select |
| D11–D13 | SPI |
| A0 | Status LED |
| A1 | Buzzer |
| A4 | I²C SDA |
| A5 | I²C SCL |

<img width="800" height="640" alt="2" src="https://github.com/user-attachments/assets/f79d8f77-7d8d-4c17-adfa-dd2f419205ce" />

<img width="811" height="512" alt="5" src="https://github.com/user-attachments/assets/79726f17-66b6-4971-a662-630800ced555" />

# Building

<img width="857" height="512" alt="4" src="https://github.com/user-attachments/assets/75384b88-7d53-4650-b540-ee949a30f155" />


1. Install the Arduino IDE.
2. Open `vic20/vic20.ino`.
3. Select **Arduino Uno**.
4. Verify and upload.
5. Wire per schematic, connect to a PAL TV, power on.

# Usage

# BASIC

Type BASIC commands as on a real VIC-20:

10 PRINT "HELLO"

20 FOR I = 1 TO 10

30 PRINT I

40 NEXT I

RUN

Sound

10 POKE 36878, 15    : REM volume

20 POKE 36876, 240   : REM pitch

30 FOR D = 1 TO 500  : NEXT D

40 POKE 36876, 0     : REM stop

RUN

# Save / Load

Press SAVE button once, wait 2 s → slot 1

Press SAVE button twice, wait 2 s → slot 2

...

Press SAVE button nine times, wait 2 s → slot 9


Same for LOAD. Bottom row shows SAVED SLOT n / LOADED SLOT n.

Clear screen : Press Escape (mapped to PETSCII CLR).

# Known limitations

  Keyboard glitch
  
  97% of the ATmega328P's flash is used.

# How it works

The USART runs in Master SPI mode and emits a raw serial bitstream that
the TV interprets as composite video. Each character is 8 bits at 8 Mbaud.

The 6502 emulation runs during vertical blanking, when the video ISR is idle.
Emulated RAM is backed by the 23LC1024 ($0400–$1DFF) and the ATmega's
internal RAM ($0000–$033F). Video RAM ($1E00–$1FFF) maps to a small array in SRAM.

char_rom.h is the standard VIC-20 character ROM, reordered.
No runtime conversion is needed.

# note:

You can also take a look at my other project, a working Commodore VIC-20
emulator that fits on a single Arduino UNO R4 Minima, 
drives a 2.42" SSD1309 OLED, reads a real PS/2 keyboard,
plays sound through the onboard DAC, and stores BASIC programs in the
MCU's internal EEPROM.

# Acknowledgements

- Author : Christian Bosch - 2026 - "Lets be honest, AI did most of the job !"
- The main code is derived from Jan Ostman Published February 3, 2014
	https://www.hackster.io/janost/the-nano-vic-20-e37b39
	https://github.com/mganthon/nanoVIC-20
(but code is buggy, not running on modern IDE v2.x, no save/load, no keybpard)
- Commodore 64 emulator sketch by Doctor Volt
	https://github.com/michalin/Arduino-C64-Emulator

# License

- If you use/modify/whatever this code, just give me some credits ,	and give this github link.
  

