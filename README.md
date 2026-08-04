# Embedded-UPI-Based-Payment-and-Access-Control-System

An embedded payment-verification system built around the **ATmega2560** microcontroller. A customer keys in an amount, gets a live UPI QR code on an OLED, pays with any UPI app, and the system auto-detects the bank's SMS receipt over GSM and beeps to confirm — no phone, app, or internet connection required on the vendor side.

> Built by **Hitanshu Parikh (23BEC155)**
---

## How It Works

1. **Amount entry & QR generation**
   - User types an amount (e.g. `25`) on a 4×4 matrix keypad.
   - Pressing `#` builds a `upi://pay?...` deep link and renders it as a QR code on the SSD1306 OLED.
2. **Payment & SMS verification**
   - The customer scans the QR with any UPI app and pays.
   - The bank's SMS receipt lands on the SIM inside the SIM800L module.
   - The firmware reads the SMS over UART using AT commands and scans it for payment keywords (`credited`, `INR`, `Rs`, `received`, etc.).
3. **Automatic buzzer alert**
   - On a keyword match, the buzzer fires and the OLED shows a success message, then resets to the amount-entry screen.

---

## Hardware

| Component | Purpose |
|---|---|
| ATmega2560 (e.g. Arduino Mega 2560 board) | Main controller |
| 4×4 matrix keypad | Amount entry |
| SSD1306 128×64 OLED (I²C) | QR code / status display |
| SIM800L GSM module | Receives bank SMS |
| Buzzer | Audible payment-confirmed alert |
| Buck converter | Steps 10 V battery down to a stable 4.0 V for the SIM800L |
| Relay / door lock (optional) | Demonstrated in the Proteus schematic for access-control use cases |

## Wiring

| Signal | ATmega2560 Pin |
|---|---|
| Keypad rows | PA0, PA2, PA4, PA6 |
| Keypad columns | PC7, PC5, PC3, PC1 |
| OLED SDA | Pin 20 (I²C SDA) |
| OLED SCL | Pin 21 (I²C SCL) |
| SIM800L TX → MCU RX2 | Pin 20 (via level shifter) |
| SIM800L RX ← MCU TX2 | Pin 21 (via level shifter) |
| Buzzer | PG1 (physical pin 40) |
| Ground | Common between battery, MCU, SIM800L, and OLED |

> ⚠️ The SIM800L runs on ~3.4–4.4 V and can draw current spikes up to 2 A during transmit bursts. Always power it from the buck converter, **not** directly from the MCU's 5 V rail, and share a common ground.

---

## Repository Structure

```
.
├── src/
│   └── main.c            # Firmware (keypad, OLED, GSM, buzzer logic)
├── include/
│   └── qrcodegen.h        # QR encoder header (see below — not bundled)
├── docs/
│   └── images/            # Wiring diagram / hardware photos / serial monitor capture
|
└── README.md
```

## Getting the QR Library

This project renders QR codes using **Nayuki's QR Code generator library (C port)**, which is not bundled in this repo due to its separate license. Before building:

1. Download `qrcodegen.h` and `qrcodegen.c` from https://github.com/nayuki/QR-Code-generator (the `c` folder).
2. Drop `qrcodegen.h` into `include/` and `qrcodegen.c` into `src/`.
3. Build as normal — the Makefile picks up all `.c` files in `src/` automatically.

## Building & Flashing

Requires `avr-gcc`, `avr-libc`, and `avrdude` (install via `apt install gcc-avr avr-libc avrdude` on Debian/Ubuntu, or the Arduino IDE toolchain).

```bash
# Build firmware.hex
make

# Flash to an Arduino Mega 2560 over USB (adjust PORT/PROGRAMMER as needed)
make flash PORT=/dev/ttyUSB0
```

On Windows/macOS, substitute the correct serial port (`COMx` / `/dev/cu.usbmodemXXXX`). If flashing a bare ATmega2560 with an ISP programmer instead of the bootloader, change `PROGRAMMER` in the `Makefile` accordingly (e.g. `avrispmkii`, `usbasp`).

## Configuration

Edit these constants in `src/main.c` before deploying:

| Constant / value | Location | Purpose |
|---|---|---|
| `pa=yourvpa@bank` | `generate_and_display_qr()` | Replace with your real UPI VPA (payee address) |
| `9600` | `uart_init()` / `gsm_init()` | Serial baud rates for debug UART and GSM UART |
| Payment keywords | `check_gsm_message()` | Adjust to match your bank's actual SMS wording |

## Simulation (Proteus)

The design was first validated in **Proteus** before hardware bring-up:

- Keypad input and amount display worked as expected.
- QR logic executed correctly (Proteus can't render the actual QR matrix, so it shows placeholder patterns).
- UART communication with a simulated SIM800L block returned AT responses correctly.
- The buzzer/LED indicator activated on a simulated "Payment Verified" event.
- The OLED correctly cycled through `Enter Amount` → `QR Generated` → `Unlocked` states.

Add your `.pdsprj` file and simulation screenshots to `docs/` if you want them versioned alongside the firmware.

## Known Limitations

- **Digit-only OLED font** — the bundled 5×7 font only supports characters `0`–`9`; any UI text (`"Enter Amount:"`, `"Unlocked"`, etc.) drawn today won't actually render letters. Swap in a full ASCII font (e.g. Adafruit's `glcdfont.c`) if you need readable labels.
- **Keyword-based SMS matching** — the firmware checks for generic words like `credited`/`Rs`/`INR` rather than parsing and verifying the exact paid amount against what the user entered. This is fine for a lab demo but is **not** safe for production: a stray promotional SMS containing "Rs" could trigger a false positive. Consider parsing the amount out of the SMS and comparing it to `amount[]` before beeping.
- **No transaction/reference-ID de-duplication** — replaying or resending the same SMS (or a second unrelated credit) will re-trigger the buzzer.
- **Debounce is a fixed delay (`_delay_ms(120)`)** in `keypad_getkey()`, which blocks the main loop; fine for a demo, not ideal for responsiveness at scale.
- **Hardcoded VPA in source** — move `pa=yourvpa@bank` into a config header (e.g. `secrets.h`, already excluded via `.gitignore`) rather than committing it directly if you fork this for real deployment.

## Roadmap Ideas

- [ ] Full alphanumeric OLED font
- [ ] Amount verification against the parsed SMS body (not just keyword matching)
- [ ] Transaction/reference-ID tracking to prevent duplicate triggers
- [ ] Interrupt-driven keypad scanning instead of polling
- [ ] Persist last transaction to EEPROM for audit/debug purposes
- [ ] Optional relay/door-lock actuation for access-control use cases (vending machines, lockers, gates)
