// VIC-20 emulator with ARDUINO UNO + EEPROM + SRAM
// Christian BOSCH - 2026 (lets be honest, AI did most of the job)
// based on :
// JAN OSTMAN "THE NANO VIC-20" - 2014
//   (no load/save, no keyboard, no sound)
// DOCTOR VOLT (MICHALIN) "Arduino sketch that emulates a Commodore 64" - 2021
//   (is a C64 emulation, no load/save, no sound)
//
// Original 1980 VIC-20 vs this 2026 version :
// 6502 microprocessor            -> 6502 code emulated
// CBM basic V2 in ROM            -> CBM basic V2 in 23LC1024 serial SRAM
// integrated keyboard            -> external PS/2 US keyboard
// TV composite output is VIC-I   -> code generated TV composite output
// save/load using cassette       -> save/load with 24LC512 serial EEPROM (9 slots)
// sound is VIC-I chip 4 channels -> simple sound with passive buzzer
// speed is 100%                  -> 10% speed (it's an Arduino UNO !)
//

#include <util/delay.h>
#include "char_rom.h"
#include <Wire.h>
#include "keyboard.h"

#include <avr/io.h>
#include <avr/pgmspace.h>
#include <avr/interrupt.h>
#include <avr/sleep.h>

#include <SPI.h>
#define SRAM_CS 10

//----------------- VideoBlaster definitions -----------------

#define HSYNC     126
#define LINES     262
#define START_L   41
#define CPL       22
#define ROWS      23

#define SYN_DD_REG DDRD
#define SYNCPIN   PD2
#define BLNKPIN   PD3

#define END_L     (START_L + ROWS * 8 - 1)

#define LED_PIN A0
#define BUZZER_PIN A1

volatile byte VBE = 0;

unsigned int scanline = 0;
unsigned int videoptr = 0;

#define DELAY500 asm("nop\n nop\n nop\n nop\n nop\n nop\n nop\n nop\n");

#define VSYNCLEN 8

const byte MSPIM_SCK = 4;
const byte MSPIM_SS  = 5;

//--------------------------------------------------------------

char videomem[512];
uint8_t RAM[832];

// ---- Multi-slot save/load state ----
volatile uint8_t frame_count = 0;
uint8_t save_presses = 0;
uint8_t load_presses = 0;
uint8_t save_last_press_frame = 0;
uint8_t load_last_press_frame = 0;

// ---- Status messages (screen codes, not PETSCII) ----
const char msg_saved[]  PROGMEM = { 19, 1, 22, 5, 4, 32, 19, 12, 15, 20, 32, 0 };
const char msg_loaded[] PROGMEM = { 12, 15, 1, 4, 5, 4, 32, 19, 12, 15, 20, 32, 0 };

extern "C" {
    uint16_t get_pc(void);
    void     set_pc(uint16_t v);
    uint8_t  get_sp(void);
    void     set_sp(uint8_t v);
    uint8_t  get_a(void);
    void     set_a(uint8_t v);
    uint8_t  get_x(void);
    void     set_x(uint8_t v);
    uint8_t  get_y(void);
    void     set_y(uint8_t v);
    uint8_t  get_status(void);
    void     set_status(uint8_t v);
}

void blink_led(void) {
    digitalWrite(LED_PIN, HIGH);
    _delay_ms(30);
    digitalWrite(LED_PIN, LOW);
}

// Display a status message on the bottom row of the screen.
// prefix is a PROGMEM string of screen codes; slot is 1-9.
void show_status(const char *prefix, uint8_t slot) {
    uint16_t pos = 484;   // row 22, column 0
    char c;
    while ((c = pgm_read_byte(prefix++)) != 0) {
        if (pos < 506) videomem[pos++] = c;
    }
    if (pos < 506) videomem[pos++] = 48 + slot;   // screen code for digit
    while (pos < 506) {
        videomem[pos++] = 32;                     // space
    }
}

// Clear the status message line.
void clear_status(void) {
    for (uint16_t pos = 484; pos < 506; pos++) {
        videomem[pos] = 32;
    }
}

void on_keypressed(uint8_t code) {
    if (code == 0) return;
    clear_status();
    if (RAM[0xC6] >= 10) return;
    RAM[0x0277 + RAM[0xC6]] = code;
    RAM[0xC6] = RAM[0xC6] + 1;
}

#define EEPROM_I2C 0x50

void ee_write_page(uint16_t addr, byte *data, uint8_t count) {
    uint8_t page_remaining = 128 - (addr & 0x7F);
    if (count > page_remaining) {
        count = page_remaining;
    }

    Wire.beginTransmission(EEPROM_I2C);
    Wire.write((uint8_t)(addr >> 8));
    Wire.write((uint8_t)(addr & 0xFF));
    for (uint8_t i = 0; i < count; i++) {
        Wire.write(data[i]);
    }
    Wire.endTransmission();
    _delay_ms(10);
}

void ee_write_buf(uint16_t addr, byte *data, uint16_t count) {
    while (count > 0) {
        uint8_t chunk = count;
        if (chunk > 16) chunk = 16;
        uint8_t page_remaining = 128 - (addr & 0x7F);
        if (chunk > page_remaining) chunk = page_remaining;

        ee_write_page(addr, data, chunk);
        addr += chunk;
        data += chunk;
        count -= chunk;
    }
}

void ee_read_buf(uint16_t addr, byte *buf, uint16_t count) {
    uint16_t done = 0;
    while (done < count) {
        uint16_t chunk = count - done;
        if (chunk > 30) chunk = 30;

        Wire.beginTransmission(EEPROM_I2C);
        Wire.write((uint8_t)((addr + done) >> 8));
        Wire.write((uint8_t)((addr + done) & 0xFF));
        Wire.endTransmission();
        Wire.requestFrom((uint8_t)EEPROM_I2C, (uint8_t)chunk);
        for (uint16_t i = 0; i < chunk; i++) {
            buf[done + i] = Wire.read();
        }
        done += chunk;
    }
}

#define SAVE_SIZE 6999

void save_state(uint8_t slot) {
    uint16_t base = (uint16_t)slot * SAVE_SIZE;
    uint16_t off = base;

    TIMSK0 &= ~_BV(OCIE0A);
    VBE = 1;

    for (uint16_t i = 0; i < 832; i += 16) {
        uint8_t n = (832 - i < 16) ? (832 - i) : 16;
        ee_write_buf(off, RAM + i, n);
        off += n;
    }

    for (uint16_t i = 0; i < 512; i += 16) {
        uint8_t n = (512 - i < 16) ? (512 - i) : 16;
        ee_write_buf(off, (byte *)videomem + i, n);
        off += n;
    }

    for (uint16_t i = 0; i < 5632; i += 16) {
        uint8_t n = (5632 - i < 16) ? (5632 - i) : 16;
        byte buf[16];
        for (uint8_t j = 0; j < n; j++) {
            buf[j] = sram_read(i + j);
        }
        ee_write_buf(off, buf, n);
        off += n;
    }

    byte regs[7];
    uint16_t pc_val = get_pc();
    regs[0] = pc_val & 0xFF;
    regs[1] = (pc_val >> 8) & 0xFF;
    regs[2] = get_sp();
    regs[3] = get_a();
    regs[4] = get_x();
    regs[5] = get_y();
    regs[6] = get_status();
    ee_write_buf(off, regs, 7);

    TIFR0 |= _BV(OCF0A);
    TIMSK0 |= _BV(OCIE0A);
    VBE = 0;

    show_status(msg_saved, slot + 1);
    blink_led();
}

void load_state(uint8_t slot) {
    uint16_t base = (uint16_t)slot * SAVE_SIZE;
    uint16_t off = base;

    TIMSK0 &= ~_BV(OCIE0A);
    VBE = 1;

    for (uint16_t i = 0; i < 832; i += 16) {
        uint8_t n = (832 - i < 16) ? (832 - i) : 16;
        ee_read_buf(off, RAM + i, n);
        off += n;
    }

    for (uint16_t i = 0; i < 512; i += 16) {
        uint8_t n = (512 - i < 16) ? (512 - i) : 16;
        ee_read_buf(off, (byte *)videomem + i, n);
        off += n;
    }

    for (uint16_t i = 0; i < 5632; i += 16) {
        uint8_t n = (5632 - i < 16) ? (5632 - i) : 16;
        byte buf[16];
        ee_read_buf(off, buf, n);
        for (uint8_t j = 0; j < n; j++) {
            sram_write(i + j, buf[j]);
        }
        off += n;
    }

    byte regs[7];
    ee_read_buf(off, regs, 7);
    set_pc((uint16_t)regs[0] | ((uint16_t)regs[1] << 8));
    set_sp(regs[2]);
    set_a(regs[3]);
    set_x(regs[4]);
    set_y(regs[5]);
    set_status(regs[6]);

    TIFR0 |= _BV(OCF0A);
    TIMSK0 |= _BV(OCIE0A);
    VBE = 0;

    show_status(msg_loaded, slot + 1);
    blink_led();
}

// ---- Write-back buffer for SPI SRAM ----
#define WBUF_SIZE 4
uint16_t wbuf_addr[WBUF_SIZE];
byte     wbuf_data[WBUF_SIZE];
volatile byte wbuf_count = 0;

extern "C" {
    void exec6502();
    void reset6502();

    void sram_init() {
        pinMode(SRAM_CS, OUTPUT);
        digitalWrite(SRAM_CS, HIGH);
        SPI.begin();

        digitalWrite(SRAM_CS, LOW);
        SPI.transfer(0x01);
        SPI.transfer(0x40);
        digitalWrite(SRAM_CS, HIGH);
    }

    void sram_write(uint16_t addr, byte data) {
        digitalWrite(SRAM_CS, LOW);
        SPI.transfer(0x02);
        SPI.transfer(0x00);
        SPI.transfer((addr >> 8) & 0xFF);
        SPI.transfer(addr & 0xFF);
        SPI.transfer(data);
        digitalWrite(SRAM_CS, HIGH);
    }

    byte sram_read(uint16_t addr) {
        digitalWrite(SRAM_CS, LOW);
        SPI.transfer(0x03);
        SPI.transfer(0x00);
        SPI.transfer((addr >> 8) & 0xFF);
        SPI.transfer(addr & 0xFF);
        byte d = SPI.transfer(0);
        digitalWrite(SRAM_CS, HIGH);
        return d;
    }

    void flush_writes() {
        byte n = wbuf_count;
        wbuf_count = 0;
        for (byte i = 0; i < n; i++) {
            sram_write(wbuf_addr[i], wbuf_data[i]);
        }
    }

    void writeEEPROM(unsigned int eeaddress, byte data) {
        // VIC-20 sound
        if (eeaddress >= 36874 && eeaddress <= 36878) {
            if (data == 0) {
                noTone(BUZZER_PIN);
            } else if (eeaddress != 36878) {
                if (data >= 128) {
                    uint16_t freq = 4000 - ((data - 128) * 3920UL) / 126;
                    tone(BUZZER_PIN, freq);
                }
            }
            return;
        }

        // Video RAM: $1E00-$1FFF
        if ((eeaddress & 0xFE00) == 0x1E00) {
            videomem[eeaddress & 0x1FF] = data;
            return;
        }

        // Main RAM: $0400-$1DFF
        if (eeaddress > 1023 && eeaddress < 7680) {
            uint16_t addr = eeaddress - 1024;
            for (byte i = 0; i < wbuf_count; i++) {
                if (wbuf_addr[i] == addr) {
                    wbuf_data[i] = data;
                    return;
                }
            }
            if (wbuf_count >= WBUF_SIZE) {
                flush_writes();
            }
            wbuf_addr[wbuf_count] = addr;
            wbuf_data[wbuf_count] = data;
            wbuf_count++;
            return;
        }
        return;
    }

    byte readEEPROM(unsigned int eeaddress) {
        if ((eeaddress & 0xFE00) == 0x1E00) {
            return videomem[eeaddress & 0x1FF];
        }
        if (eeaddress > 1023 && eeaddress < 7680) {
            uint16_t addr = eeaddress - 1024;
            for (byte i = 0; i < wbuf_count; i++) {
                if (wbuf_addr[i] == addr) {
                    return wbuf_data[i];
                }
            }
            return sram_read(addr);
        }
        return 0xFF;
    }
}

// ---------------------------------------------------------------
// Video line interrupt
// ---------------------------------------------------------------
ISR(TIMER0_COMPA_vect) {

    bitSet(SYN_DD_REG, BLNKPIN);
    DELAY500;
    DELAY500;
    DELAY500;

    bitSet(SYN_DD_REG, SYNCPIN);

    scanline >= LINES ? scanline = 0 : scanline++;
    if (scanline == 0) frame_count++;

    if (scanline >= VSYNCLEN) {

        DELAY500;
        bitClear(SYN_DD_REG, SYNCPIN);

        DELAY500;
        bitClear(SYN_DD_REG, BLNKPIN);

        if (scanline == START_L - 5) {
            KBDISABLE();
        }

        if ((scanline >= START_L) && (scanline <= END_L)) {

            uint16_t line = scanline - START_L;

            delayMicroseconds(5);

            const register byte *basePtr = &charROM[(line & 0x07) * 128];
            videoptr = (line >> 3) * CPL;
            register byte *messagePtr = (byte *)&videomem[videoptr];

            for (uint16_t p = 0; p < CPL; p++) {
                UDR0 = pgm_read_byte(basePtr + ((*messagePtr++) & 0x7F));
                DELAY500;
                DELAY500;
            }
            while (!(UCSR0A & _BV(UDRE0))) {}
        }
        else if (scanline == 30) {
            VBE = 1;
        }
        else if (scanline == 235) {
            VBE = 0;
        }
        else if (scanline == 20) {
            // Simple non-blinking cursor.
            // Only shown when the keyboard buffer is empty (KERNAL idle),
            // so it doesn't interfere with command entry.
            static uint16_t last_pos = 0xFFFF;
            if (!RAM[0xCC] && RAM[0xC6] == 0) {
                uint16_t pos = ((RAM[0xD2] << 8) + RAM[0xD1] - 7680) + RAM[0xD3];
                if (pos < 506) {
                    if (pos != last_pos && last_pos < 506) {
                        if (videomem[last_pos] == 102) {
                            videomem[last_pos] = 32;
                        }
                    }
                    videomem[pos] = 102;
                    last_pos = pos;
                }
            } else {
                // Keyboard buffer non-empty: erase any lingering cursor
                if (last_pos < 506 && videomem[last_pos] == 102) {
                    videomem[last_pos] = 32;
                }
                last_pos = 0xFFFF;
            }
        }
        else if (scanline == END_L + 5) {
            KBENABLE();
        }
    }
}

// ---------------------------------------------------------------
void setup() {
    pinMode(MSPIM_SS, OUTPUT);
    pinMode(MSPIM_SCK, OUTPUT);
    pinMode(2, OUTPUT);
    pinMode(3, OUTPUT);
    pinMode(6, INPUT_PULLUP);
    pinMode(7, INPUT_PULLUP);
    pinMode(LED_PIN, OUTPUT);
    digitalWrite(LED_PIN, LOW);
    pinMode(BUZZER_PIN, OUTPUT);
    digitalWrite(BUZZER_PIN, HIGH);
    Wire.begin();

    sram_init();
    kb_init();

    UCSR0C = _BV(UMSEL00) | _BV(UMSEL01);
    UBRR0  = 0;
    bitSet(UCSR0B, TXEN0);

    TCCR0A = 0;
    TCCR0B = 0;
    TCNT0  = 0;
    OCR0A  = HSYNC;
    bitSet(TCCR0A, WGM01);
    bitSet(TCCR0B, CS01);
    set_sleep_mode(SLEEP_MODE_IDLE);

    pinMode(1, OUTPUT);
    digitalWrite(1, LOW);

    for (int i = 0; i < 512; i++) {
        videomem[i] = 32;
    }

    reset6502();
    exec6502();
    for (unsigned long i = 0; i < 150000UL; i++) {
        exec6502();
    }

    bitSet(TIMSK0, OCIE0A);
    sei();
}

// ---------------------------------------------------------------
byte prev_save = HIGH;
byte prev_load = HIGH;

void loop() {
    while (VBE == 1) {
        sleep_enable();
        sleep_cpu();
        sleep_disable();
    }

    byte save_btn = digitalRead(6);
    byte load_btn = digitalRead(7);

    if (prev_save == HIGH && save_btn == LOW) {
        save_presses++;
        if (save_presses > 9) save_presses = 9;
        save_last_press_frame = frame_count;
        while (digitalRead(6) == LOW);
    }
    prev_save = save_btn;

    if (prev_load == HIGH && load_btn == LOW) {
        load_presses++;
        if (load_presses > 9) load_presses = 9;
        load_last_press_frame = frame_count;
        while (digitalRead(7) == LOW);
    }
    prev_load = load_btn;

    if (save_presses > 0 &&
        (uint8_t)(frame_count - save_last_press_frame) > 120) {
        flush_writes();
        save_state(save_presses - 1);
        save_presses = 0;
    }

    if (load_presses > 0 &&
        (uint8_t)(frame_count - load_last_press_frame) > 120) {
        flush_writes();
        load_state(load_presses - 1);
        load_presses = 0;
    }

    exec6502();
}