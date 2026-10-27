# Firmware

The **Tinta** firmware is a compact, bare-metal C application for the **Raspberry Pi Pico W**, designed for ultra-low-power **ePaper signage**. It supports over-the-air firmware updates over Wi-Fi, flash-persistent configuration, and content fetched over Wi-Fi from whichever back end the selected use case targets — Seatsurfing room booking, CCU-Historian time series, or Homematic status.

## Key Features

- No RTOS
  Runs on bare metal using the official Pico SDK, cyw43_arch and lwIP for the Wi-Fi web server and client queries. Minimal runtime overhead, deterministic behaviour, and low power consumption. The device is powered down completely between updates, so every wake starts from a cold boot — there is no long-running state to drift or fragment. Field units have passed 5000 wake cycles on a single set of cells; see the [circuit README](../hardware/circuit/README.md) for measured data.

- Dual-slot bootloader (Slot 0 and Slot 1)
  Enables safe OTA / WIFI updates: new firmware is flashed to the inactive slot and activated only after verification by the device (magic word and crc32 check).

- Power-optimized architecture
  A P-MOSFET fully disconnects power when inactive. Three sources wake the device: an RTC alarm (DS3231 or RV-3028, auto-detected at runtime), a pushbutton, or an NFC field event.

- Configuration in flash
  Wi-Fi credentials, Seatsurfing settings, and display behavior are stored in dedicated flash regions, separate from firmware.

- Web-based configuration portal
  Device features an access point mode, entered by holding pushbutton 3 alone or all three pushbuttons while it powers on. Configuration is done via browser (Wi-Fi, API, room, clock, ...).

- Seatsurfing API integration
  Fetches live booking data and displays current room status. Multiple spaces per display supported.

- Flexible build system
  Bootloader, slot binaries and the factory-configuration image are generated automatically via `build.sh`; `create_uf2.sh` packs them into a single USB-flashable image.

- ePaper display support
  Waveshare 4.2″ or 7.5″ panels, selected at build time. The content persists without power.


---
# Building the Tinta Firmware

Steps to clone, configure, and build the firmware.

## 1. Clone the repository

```bash
git clone https://github.com/c0de111/tinta.git
cd tinta/firmware/c
```

## 2. Build

### Basic Build (SeatSurfing - Default)
```bash
./build.sh
```

### Use Case Selection
The firmware supports multiple use cases that can be selected at build time:

```bash
# SeatSurfing room booking (default)
./build.sh --seatsurfing

# Historian time-series data visualization
./build.sh --historian

# Homematic home-automation status
./build.sh --homematic

# Weathermap (experimental)
./build.sh --weathermap

# Standalone NFC antenna tuning firmware
./build.sh --st25-tune

# Template for new use cases
./build.sh --new-usecase

# Show all available options
./build.sh --help
```

### Build Output
This should give you (among others) the following files (numbers show approximate size):

```bash
46552   inki_bootloader.bin
 8332   inki_default_config.bin
680012  inki_slot0.bin          # Size varies by use case: SeatSurfing ~681KB, Historian ~689KB
680012  inki_slot1.bin
```

## 3. Write the firmware

### Over USB — no programmer needed

`./create_uf2.sh` (run after `./build.sh`) packs the bootloader, slot 0, and the default
configuration into a single `.uf2`, named after the selected use case. Prebuilt ones are attached
to every release.

Hold BOOTSEL while plugging the board into USB, then copy the `.uf2` onto the `RPI-RP2` drive that
appears. Because the image covers all three flash regions, this works on a factory-fresh Pico W —
no SWD, no debug probe. Everything after that can be done over the web interface, including
firmware updates.

```bash
./build.sh --seatsurfing
./create_uf2.sh              # -> build/tinta_seatsurfing.uf2
```

### Over SWD — for development

`flash.sh` writes the regions individually, which is what you want while working on the firmware:
it can rewrite slot 0 alone and leave your configuration in place.

⚠️ Check which sections are enabled before running it. The default-configuration section is a
factory reset and erases all user settings.

```bash
./flash.sh
```

## Notes

- Make sure the correct version of the Pico SDK is properly included via `pico_sdk_import.cmake`.

---

## Debug Logging Sample Output for a single wake up cycle
```bash

[3849 ms] Trying to connect to ssid ... Attempt 1
[6642 ms] Trying to connect to ssid ... Attempt 2
[6643 ms] Connected to Wi-Fi successfully.
[6643 ms] Constructed HTTP Header:
GET /location/5230035c-94ce-4f3c-b112-ebc6afcb78b9/space/222c2a66-6e66-4825-8064-11a6c3ecf91f/availability HTTP/1.0
Host: seatsurfing.io
Authorization: Basic ZThjNTA4NzktasdYmQzZi00OTMtZDJjZjMyZDdlZTdmX2VzaWdauQHNlYXRzdXJmaW5fanLmxvY2FsOnQ2R1RCaHozY1dUN0JcFTDROSEVWVTJ0cWRNcDZnMnRu
d

[6644 ms] Data transmission in progress...
[6645 ms] 50 ms wait time for header/body #: [6695 ms]  [6745 ms]  [6795 ms]  [6845 ms]  [6896 ms]  [6946 ms]  [6996 ms]  [7046 ms]  [7096 ms]  [7146 ms]  [7197 ms]  [7247 ms]  [7297 ms]  [7347 ms]  [7397 ms]  [7447 ms]  [7498 ms]  [7548 ms]  [7598 ms]  [7648 ms]  [7698 ms]  [7749 ms]  [7799 ms]  [7849 ms]  [7899 ms]  [7949 ms]  [7999 ms]  [8050 ms]  [8100 ms]  [8150 ms]  [8200 ms]  [8250 ms]  [8300 ms]  [8350 ms]  [8401 ms]  [8451 ms]  [8501 ms]  [8551 ms]  [8601 ms]  [8651 ms]  [8702 ms]  [8752 ms]  [8802 ms]  [8852 ms]  [8902 ms]  [8952 ms]  [9003 ms]  [9053 ms]  [9103 ms]  [9153 ms]  [9203 ms]  [9253 ms]  [9304 ms]  [9354 ms]  [9404 ms]  [9454 ms]  [9505 ms]  [9555 ms]  [9567 ms] Buffer= HTTP/1.0 200 OK
Access-Control-Allow-Headers: *
Access-Control-Allow-Methods: POST, GET, PUT, DELETE, OPTIONS
Access-Control-Allow-Origin: *
Access-Control-Expose-Headers: X-Object-Id, X-Error-Code, Content-Length, Content-Type
Content-Type: application/json
Date: Sun, 03 Aug 2025 13:27:48 GMT
Content-Length: 338

[{"id":"222c2a66-6e66-4825-8064-11a6c3ecf91f","available":true,"locationId":"5230035c-94ce-4f3c-b112-ebc6afcb78b9","name":"Conference 1","x":990,"y":76,"width":204,"height":70,"rotation":0,"requireSubject":false,"attributes":null,"approverGroupIds":null,"allowedBookerGroupIds":null,"bookings":[],"allowed":true,"approvalRequired":false}]
[9605 ms]  [9605 ms] Parsed Content-Length: 338
[9606 ms] Received full JSON body (338 bytes)
[9608 ms] ✅ JSON response complete - Wi-Fi off.
[9609 ms] Disabling watchdog for ePaper setup...
[9609 ms] Initializing Waveshare 4.2-inch ePaper...
[12167 ms] Re-enabling watchdog...
[12167 ms] Creating new image...
[12168 ms] Selecting image...
[12169 ms] ePaper setup completed.
[12169 ms] render_page
[12174 ms] Flash-Logo gefunden: 104x95 px, 1235 bytes
[12184 ms] epaper_finalize_and_powerdown (display epaper page)...
[12184 ms] EPD_Display called for epaper type: 2
[14530 ms] Entering ePaper sleep mode for type: 2
[14930 ms] Shutting down the ePaper module...
[14930 ms] ...System shutting down.
[14932 ms] Alarm2 set for 00:24 (RTC time)
```

---

## ST25 NFC Integration

Optional NFC wake and command interface using the ST25DV04KC passive NFC tag (I2C + RF dual interface). Present on the L2 PCB; auto-detected at runtime via I2C probe — boards without ST25 are unaffected.

### Hardware

| Signal     | GPIO | Function                                          |
|------------|------|---------------------------------------------------|
| VCC_ST25   | GP18 | Enables 3.3V to ST25 (off during sleep = 0A)     |
| V_EH_ST25  | GP28 | ADC2 — RF energy harvesting voltage (tune/sense)  |
| SDA / SCL  | GP20 / GP21 | Shared I2C bus (with RTC)                  |
| GPO        | —    | Wired to GATE circuit via Schottky diode          |

**Power architecture:** ST25 VCC is hard off during sleep (GP18 floats). The tag is powered exclusively by the phone's RF field. GPO fires RF-powered on EEPROM write, pulling GATE low through the shared wake circuit (same path as RTC alarm). Standby draw: 0A.

### Wake Source Detection

All wake sources (RTC alarm, button press, NFC GPO) share the same GATE line via Schottky OR. Once the MCU boots, the original trigger signal is gone. The firmware determines the wake source from four independent latched signals read at boot:

| Signal | Source | Latched? | Meaning |
|--------|--------|----------|---------|
| `IT_STS_Dyn` | ST25DV register | Read-clears | Any recent RF activity (e.g. `0x10` = FIELDRISING) |
| INKI magic in EEPROM | ST25DV EEPROM | Until firmware clears | Valid 16-byte command from Android app |
| Pushbutton GPIOs | Hardware straps | Held by user | Button bitmask 0-7 (0 = no button) |
| RTC alarm flag | RV-3028 / DS3231 | Set by RTC hardware | Scheduled wake from previous cycle |

**Decision tree (implemented in `main()`):**

```
st25.request_valid?
├── YES → intentional NFC wake → handle opcode, proceed with page
└── NO
    ├── pushbutton != 0 → button wake → normal cycle (page selected by button)
    │   (codes 4 and 7 both select AP/setup mode)
    └── pushbutton == 0
        ├── it_sts != 0 AND !alarm_flag → spurious NFC (generic tap, no payload)
        │   → LED 3x blink, power off immediately
        ├── alarm_flag set → RTC scheduled wake → normal cycle (page 0)
        └── it_sts == 0, !alarm_flag → default wake → normal cycle (page 0)
```

**NFC EEPROM read with retry:** When `it_sts != 0` (RF activity detected), the firmware retries the EEPROM read up to 10 times at 100ms intervals (~1 second window). This compensates for the timing gap between RF field detection (~0ms) and the phone completing the EEPROM write (~200-400ms for NFC negotiation + write). When `it_sts == 0` (button/RTC wake), a single read is performed with zero delay.

### NFC Wake Flow

1. Phone taps antenna → RF field powers ST25
2. ST25 GPO fires → pulls GATE low → Pico powers on
3. Firmware holds GP22 HIGH, enables ST25 VCC via GP18
4. Reads `IT_STS_Dyn` (RF activity flags) and `EH_CTRL_Dyn` (field state)
5. Retries EEPROM read (up to 1s) while phone completes NFC write
6. Validates INKI magic + payload fields
7. Proceeds with requested operation (currently: default page cycle)
8. At shutdown: clears EEPROM request slot (zeros), powers off ST25
9. Sets RTC alarm, releases GP22 → system powers down

### INKI Request Format (16 bytes)

| Offset | Size | Field          | Description                    |
|--------|------|----------------|--------------------------------|
| 0-3    | 4    | magic          | ASCII `"INKI"`                 |
| 4      | 1    | version        | Must be `1`                    |
| 5      | 1    | opcode         | Command code                   |
| 6-7    | 2    | duration_min   | Little-endian, 1-1440 minutes  |
| 8-11   | 4    | unix_seconds   | Little-endian UNIX timestamp   |
| 12-15  | 4    | nonce          | Little-endian random value     |

**Opcodes:**
- `0x01` — Immediate refresh (page 0)
- `0x11` — LED slow blink (test)
- `0x12` — LED fast blink (test)

The companion Android app, [wake-electronics/tinta-tap](https://github.com/wake-electronics/tinta-tap), produces this format.

### ST25 Antenna Tuning Helper

Standalone firmware for characterizing RF coupling during antenna matching. Measures V_EH via ADC and renders a live ANSI terminal graph over USB serial.

```bash
# Build
cd firmware/c
./build.sh --st25-tune

# Flash via UF2 (hold BOOTSEL + plug USB, then copy)
cp build/inki_st25_tune.uf2 /media/$USER/RPI-RP2/

# Monitor
sudo tio /dev/ttyACM0
```

**Workflow:**
1. Wait for baseline capture (~4 seconds, keep RF away)
2. Tap phone to antenna → live bar graph shows V_EH delta in mV
3. Adjust trimmer capacitors to maximize peak voltage
4. Color-coded bars: blue (baseline) → green → yellow → red (strong coupling)
5. Session peak tracking shows maximum delta per phone tap

**Note:** The tune firmware overwrites the entire flash (no bootloader layout). Flash the normal Tinta firmware back when done.

### Source Files

| File | Purpose |
|------|---------|
| `c/third_party/st25dv/` | ST25DV driver (register I/O, from NFC_exploration) |
| `c/st25_io.c / .h` | Boot check, bus adapter, GPO config, request read/clear, retry logic |
| `c/st25_tune_main.c` | Standalone antenna tuning firmware |
| `c/config.h` | Pin definitions (ST25_VCC_EN_PIN, ST25_VEH_ADC_INPUT) |

### Implementation Status

- [x] ST25 driver ported and integrated
- [x] Standalone antenna tune firmware (`--st25-tune`)
- [x] Wake source detection (IT_STS_Dyn + EEPROM + buttons + RTC alarm flag)
- [x] EEPROM read with retry for NFC write timing compensation
- [x] Spurious NFC rejection (generic tap → 3x blink, power off)
- [x] EEPROM request clearing after processing
- [x] AP mode safety (RTC alarm disabled, debug mode switch)
- [ ] NFC opcode routing (soft pushbutton / page selection from app)
- [ ] Webserver V_EH live feed

---
