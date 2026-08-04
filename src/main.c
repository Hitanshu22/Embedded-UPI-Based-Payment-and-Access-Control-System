/*
 * ============================================================================
 *  IoT-Based UPI Payment System — ATmega2560 Firmware
 * ============================================================================
 *  Authors : Hitanshu Parikh (23BEC155)
 *  Target  : ATmega2560 (16 MHz)
 *
 *  Description:
 *    Reads an amount from a 4x4 matrix keypad, builds a UPI deep-link string,
 *    renders it as a QR code on an SSD1306 OLED (I2C), listens for the bank's
 *    payment-confirmation SMS via a SIM800L GSM module (UART2), and drives a
 *    buzzer on successful payment detection.
 *
 *  Dependencies:
 *    - qrcodegen.h / qrcodegen.c  (Project Nayuki's QR Code generator,
 *      C port). Not bundled here — see README.md for setup instructions.
 *
 *  NOTE: This file is a cleaned-up / bug-fixed version of the original
 *  project source (missing closing braces on main() were restored, and
 *  the digit-only 5x7 font is documented as a known limitation — see
 *  README.md "Known Limitations").
 * ============================================================================
 */

#include <avr/io.h>
#include <util/delay.h>
#include <string.h>
#include <stdio.h>
#include "qrcodegen.h"

#define F_CPU 16000000UL

#define MAX_DIGITS      10
#define SSD1306_ADDR    0x3C
#define SCREEN_WIDTH    128
#define SCREEN_HEIGHT   64
#define BUZZER_PIN      PG1   /* Physical pin 40 = PG1 */

/* ---------------- Globals ---------------- */
static uint8_t oled_buffer[SCREEN_WIDTH * SCREEN_HEIGHT / 8];
static char    amount[MAX_DIGITS + 1];
static uint8_t amount_index = 0;   /* renamed from `index` to avoid shadowing */
static uint8_t qr_displayed = 0;

/* ---------------- 5x7 digit font (0-9 only) ---------------- */
static const uint8_t font5x7[][5] = {
    {0x3E,0x51,0x49,0x45,0x3E}, /* 0 */
    {0x00,0x42,0x7F,0x40,0x00}, /* 1 */
    {0x42,0x61,0x51,0x49,0x46}, /* 2 */
    {0x21,0x41,0x45,0x4B,0x31}, /* 3 */
    {0x18,0x14,0x12,0x7F,0x10}, /* 4 */
    {0x27,0x45,0x45,0x45,0x39}, /* 5 */
    {0x3C,0x4A,0x49,0x49,0x30}, /* 6 */
    {0x01,0x71,0x09,0x05,0x03}, /* 7 */
    {0x36,0x49,0x49,0x49,0x36}, /* 8 */
    {0x06,0x49,0x49,0x29,0x1E}  /* 9 */
};

/* ============================================================
 *  UART0 — USB Serial Monitor (debug output)
 * ============================================================ */
void uart_init(uint32_t baud) {
    uint16_t ubrr = F_CPU / 16 / baud - 1;
    DDRE  |= (1 << PE1);   /* TX0 output */
    DDRE  &= ~(1 << PE0);  /* RX0 input  */
    UBRR0H = (ubrr >> 8);
    UBRR0L = ubrr;
    UCSR0B = (1 << TXEN0) | (1 << RXEN0);
    UCSR0C = (1 << UCSZ01) | (1 << UCSZ00);
}

void uart_sendChar(char c) {
    while (!(UCSR0A & (1 << UDRE0)));
    UDR0 = c;
}

void uart_sendString(const char *str) {
    while (*str) uart_sendChar(*str++);
}

/* ============================================================
 *  UART2 — SIM800L GSM module
 * ============================================================ */
void gsm_init(uint32_t baud) {
    uint16_t ubrr = F_CPU / 16 / baud - 1;
    DDRH  |= (1 << PH1);   /* TX2 */
    DDRH  &= ~(1 << PH0);  /* RX2 */
    UBRR2H = (ubrr >> 8);
    UBRR2L = ubrr;
    UCSR2B = (1 << TXEN2) | (1 << RXEN2);
    UCSR2C = (1 << UCSZ21) | (1 << UCSZ20);
}

void gsm_sendChar(char c) {
    while (!(UCSR2A & (1 << UDRE2)));
    UDR2 = c;
}

void gsm_sendString(const char *str) {
    while (*str) gsm_sendChar(*str++);
}

uint8_t gsm_available(void) {
    return (UCSR2A & (1 << RXC2));
}

char gsm_readChar(void) {
    while (!gsm_available());
    return UDR2;
}

/* ============================================================
 *  4x4 Keypad
 * ============================================================ */
static const char keypad_map[4][4] = {
    {'1','2','3','A'},
    {'4','5','6','B'},
    {'7','8','9','C'},
    {'*','0','#','D'}
};

void keypad_init(void) {
    DDRA |= (1 << PA0) | (1 << PA2) | (1 << PA4) | (1 << PA6);   /* Rows: output */
    DDRC &= ~((1 << PC7) | (1 << PC5) | (1 << PC3) | (1 << PC1)); /* Cols: input  */
    PORTC |= (1 << PC7) | (1 << PC5) | (1 << PC3) | (1 << PC1);   /* Pull-ups     */
}

char keypad_getkey(void) {
    for (uint8_t row = 0; row < 4; row++) {
        PORTA |= (1 << PA0) | (1 << PA2) | (1 << PA4) | (1 << PA6);
        switch (row) {
            case 0: PORTA &= ~(1 << PA0); break;
            case 1: PORTA &= ~(1 << PA2); break;
            case 2: PORTA &= ~(1 << PA4); break;
            case 3: PORTA &= ~(1 << PA6); break;
        }
        _delay_us(5);

        uint8_t col;
        if      (!(PINC & (1 << PC7))) col = 0;
        else if (!(PINC & (1 << PC5))) col = 1;
        else if (!(PINC & (1 << PC3))) col = 2;
        else if (!(PINC & (1 << PC1))) col = 3;
        else continue;

        _delay_ms(120); /* crude debounce */
        return keypad_map[row][col];
    }
    return 0;
}

/* ============================================================
 *  I2C (TWI) bit-bang-free hardware driver
 * ============================================================ */
void i2c_init(void) {
    TWSR = 0;
    TWBR = 72; /* ~100kHz @ 16MHz */
}

void i2c_start(void) {
    TWCR = (1 << TWSTA) | (1 << TWEN) | (1 << TWINT);
    while (!(TWCR & (1 << TWINT)));
}

void i2c_stop(void) {
    TWCR = (1 << TWSTO) | (1 << TWEN) | (1 << TWINT);
    _delay_us(10);
}

void i2c_write(uint8_t data) {
    TWDR = data;
    TWCR = (1 << TWEN) | (1 << TWINT);
    while (!(TWCR & (1 << TWINT)));
}

/* ============================================================
 *  SSD1306 OLED driver (minimal)
 * ============================================================ */
void ssd1306_cmd(uint8_t cmd) {
    i2c_start();
    i2c_write(SSD1306_ADDR << 1);
    i2c_write(0x00);
    i2c_write(cmd);
    i2c_stop();
}

void ssd1306_init(void) {
    _delay_ms(100);
    static const uint8_t cmds[] = {
        0xAE,0x20,0x00,0xB0,0xC8,0x00,0x10,0x40,0x81,0x7F,
        0xA1,0xA6,0xA8,0x3F,0xA4,0xD3,0x00,0xD5,0xF0,0xD9,
        0x22,0xDA,0x12,0xDB,0x20,0x8D,0x14,0xAF
    };
    for (uint8_t i = 0; i < sizeof(cmds); i++) ssd1306_cmd(cmds[i]);
}

void ssd1306_clearBuffer(void) {
    memset(oled_buffer, 0, sizeof(oled_buffer));
}

void ssd1306_update(void) {
    for (uint8_t page = 0; page < 8; page++) {
        ssd1306_cmd(0xB0 + page);
        ssd1306_cmd(0x00);
        ssd1306_cmd(0x10);
        i2c_start();
        i2c_write(SSD1306_ADDR << 1);
        i2c_write(0x40);
        for (uint8_t col = 0; col < 128; col++)
            i2c_write(oled_buffer[page * 128 + col]);
        i2c_stop();
    }
}

void ssd1306_drawPixel(uint8_t x, uint8_t y) {
    if (x >= 128 || y >= 64) return;
    oled_buffer[x + (y / 8) * 128] |= (1 << (y % 8));
}

void ssd1306_drawChar(uint8_t x, uint8_t y, char c) {
    if (c < '0' || c > '9') return; /* digits only — see README limitations */
    const uint8_t *ch = font5x7[c - '0'];
    for (uint8_t i = 0; i < 5; i++)
        for (uint8_t j = 0; j < 7; j++)
            if (ch[i] & (1 << j))
                ssd1306_drawPixel(x + i, y + j);
}

void ssd1306_drawString(uint8_t x, uint8_t y, const char *str) {
    while (*str) {
        ssd1306_drawChar(x, y, *str++);
        x += 6;
        if (x > 123) break;
    }
}

/* ============================================================
 *  QR code rendering
 * ============================================================ */
void draw_qr(const uint8_t *qrcode, uint8_t size, uint8_t scale) {
    uint8_t offX = (128 - size * scale) / 2;
    uint8_t offY = (64  - size * scale) / 2;
    for (uint8_t y = 0; y < size; y++)
        for (uint8_t x = 0; x < size; x++)
            if (qrcodegen_getModule(qrcode, x, y))
                for (uint8_t i = 0; i < scale; i++)
                    for (uint8_t j = 0; j < scale; j++)
                        ssd1306_drawPixel(offX + x * scale + i, offY + y * scale + j);
}

void generate_and_display_qr(const char *amt) {
    char upi[150];
    /* NOTE: replace the VPA below with your own before deploying */
    sprintf(upi, "upi://pay?pa=yourvpa@bank&pn=Shop&am=%s&cu=INR", amt);

    uint8_t qrcode[400], temp[400];
    qrcodegen_encodeText(upi, temp, qrcode,
                          qrcodegen_Ecc_LOW, 1, 40, qrcodegen_Mask_AUTO, true);

    ssd1306_clearBuffer();
    draw_qr(qrcode, qrcodegen_getSize(qrcode), 2);
    ssd1306_update();
    qr_displayed = 1;
}

/* ============================================================
 *  UI helpers
 * ============================================================ */
void display_amount(void) {
    if (qr_displayed) return;
    ssd1306_clearBuffer();
    ssd1306_drawString(0, 0, "Enter Amount:");
    if (amount_index > 0) ssd1306_drawString(0, 16, amount);
    ssd1306_update();
}

/* ============================================================
 *  Buzzer
 * ============================================================ */
void buzzer_beep(void) {
    PORTG |= (1 << BUZZER_PIN);
    _delay_ms(700);
    PORTG &= ~(1 << BUZZER_PIN);
}

/* ============================================================
 *  GSM SMS polling / payment keyword detection
 * ============================================================ */
void check_gsm_message(void) {
    if (!gsm_available()) return;

    char buffer[400];
    memset(buffer, 0, sizeof(buffer));
    uint16_t i = 0;
    uint32_t timeout = 0;

    while (timeout < 2000 && i < sizeof(buffer) - 1) {
        if (UCSR2A & (1 << RXC2)) {
            buffer[i++] = UDR2;
            timeout = 0;
        } else {
            _delay_ms(1);
            timeout++;
        }
    }
    buffer[i] = '\0';

    uart_sendString("\r\n----- GSM MESSAGE RECEIVED -----\r\n");
    uart_sendString(buffer);
    uart_sendString("\r\n--------------------------------\r\n");

    /* Simple keyword match — see README "Known Limitations" regarding
       false positives and amount verification. */
    if (strstr(buffer, "credited") ||
        strstr(buffer, "Credit")   ||
        strstr(buffer, "Dear")     ||
        strstr(buffer, "INR")      ||
        strstr(buffer, "Rs")       ||
        strstr(buffer, "received")) {
        uart_sendString("[SYSTEM] PAYMENT DETECTED!\r\n");
        buzzer_beep();
    }
}

/* ============================================================
 *  Main
 * ============================================================ */
int main(void) {
    uart_init(9600);
    gsm_init(9600);
    keypad_init();
    i2c_init();
    ssd1306_init();

    DDRG |= (1 << BUZZER_PIN);
    PORTG &= ~(1 << BUZZER_PIN);

    ssd1306_clearBuffer();
    ssd1306_drawString(0, 0, "Enter Amount:");
    ssd1306_update();

    memset(amount, 0, sizeof(amount));

    /* GSM module boot sequence */
    gsm_sendString("AT\r");
    _delay_ms(500);
    gsm_sendString("AT+CMGF=1\r");       /* text-mode SMS */
    _delay_ms(500);
    gsm_sendString("AT+CNMI=1,2,0,0,0\r"); /* forward new SMS to UART immediately */
    _delay_ms(500);

    uart_sendString("\r\nSystem Ready...\r\n");

    while (1) {
        check_gsm_message();
        char key = keypad_getkey();

        if (key) {
            char dbg[20];
            sprintf(dbg, "Key: %c\r\n", key);
            uart_sendString(dbg);

            if (qr_displayed && (key >= '0' && key <= '9')) {
                qr_displayed = 0;
                amount_index = 0;
                memset(amount, 0, sizeof(amount));
                ssd1306_clearBuffer();
            }

            if (key >= '0' && key <= '9') {
                if (amount_index < MAX_DIGITS) amount[amount_index++] = key;
            } else if (key == '*') {
                amount_index = 0;
                memset(amount, 0, sizeof(amount));
                qr_displayed = 0;
            } else if (key == '#') {
                if (amount_index > 0) {
                    amount[amount_index] = '\0';
                    generate_and_display_qr(amount);
                }
                amount_index = 0;
                memset(amount, 0, sizeof(amount));
            }

            display_amount();
        }
    }

    return 0;
}

