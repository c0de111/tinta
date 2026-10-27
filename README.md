# Tinta

Tinta is a battery-powered ePaper sign built around the Raspberry Pi Pico W. A real-time clock
wakes it on schedule, it fetches data over Wi-Fi, redraws the screen, and then switches itself off
completely in hardware — there is no sleep mode. A push button or an NFC tap wakes it on demand.
This approach earned an Honorable Mention in the "Least Power" category of
[Hackaday's 2026 Green-Powered Challenge](https://hackaday.com/2026/05/07/congratulations-to-the-green-powered-challenge-winners/).
One set of ordinary alkaline cells lasts thousands of refresh cycles, and the ePaper keeps its
image while the device is off.

This repository holds everything needed to build one: firmware, PCB design, 3D-printable enclosure,
and an optional telemetry service. Formerly published as **inki**.

[Crowd Supply campaign](https://www.crowdsupply.com/wake-electronics/tinta) · [Hackaday logs](https://hackaday.io/project/203726-tinta-low-power-wireless-epaper-device) · [Mastodon](https://hachyderm.io/@tinta) · [MakerTube](https://makertube.net/c/tinta) · [YouTube](https://www.youtube.com/@tinta-gadget)

<p align="center">
  <a href="images/esign_4_2_1_cropped.jpg" target="_blank">
    <img src="images/esign_4_2_1_cropped.jpg" alt="Assembled 4.2-inch Tinta" width="500" style="border-radius: 8px;">
  </a>
</p>

---

## Try it on a bare Pico W

No custom hardware required — the firmware runs on a stock Pico W with the display disabled.

1. Download a prebuilt UF2 from the [latest release](https://github.com/c0de111/tinta/releases/latest):
   [seatsurfing](https://github.com/c0de111/tinta/releases/latest/download/tinta_seatsurfing.uf2) ·
   [historian](https://github.com/c0de111/tinta/releases/latest/download/tinta_historian.uf2) ·
   [homematic](https://github.com/c0de111/tinta/releases/latest/download/tinta_homematic.uf2)
2. Hold BOOTSEL while plugging in the Pico W, then copy the UF2 to the `RPI-RP2` drive.
3. The onboard LED blinks `TINTA` in Morse once it is up.
4. Connect to the Wi-Fi access point `Tinta-XXXX`. Its password is shown on the display, or on a bare Pico W in the USB serial log (`AP active: SSID=… password=…`). Then open `http://192.168.4.1`.

## Build from source

Building, flashing, use-case selection, and the debug output are documented in
**[firmware/README.md](firmware/README.md)**. In short:

```
cd firmware/c
./build.sh --seatsurfing        # or --historian, --homematic, --weathermap
```

Requires the Pico SDK 2.1.0 (`PICO_SDK_PATH`), the ARM GNU toolchain, and CMake 3.12+.
The build produces a bootloader, two firmware slots for over-the-air updates, and a default
configuration image.

## Repository layout

```
firmware/       Pico W firmware in C — build system, use cases, drivers, web interface
hardware/       KiCad schematic and PCB (circuit/), FreeCAD and STL enclosure (enclosure/)
inki-monitor/   optional Python/Flask service collecting battery and Wi-Fi telemetry
scripts/        helper scripts
images/         photographs and diagrams used in the documentation
```

Each directory has its own README with the details.

## Hardware

The [enclosure](hardware/enclosure/) is modular and 3D-printable, with a dovetail mount: the
baseplate is glued or screwed to the wall and the sign clips on, so it lifts straight off for a
battery change.

![Assembly of the 4.2-inch enclosure](hardware/enclosure/images/assembly.gif)

The [circuit](hardware/circuit/) is a custom PCB carrying the Pico W, a real-time clock, MOSFET
power switching, battery monitoring, and an optional NFC front end.

## NFC

Boards with the optional ST25DV04KC tag can be reached by a phone **while the device is switched
off**: the phone's field powers the tag, the phone writes a short command into its EEPROM, and the
tag pulls the wake line. The Pico W then boots, reads the command over I²C, and acts on it — so a
tap can refresh the display or book a desk without the device having been powered at all.

The 16-byte wire format, the wake-source decision tree, and the antenna tuning helper are described
in [firmware/README.md](firmware/README.md) and [firmware/c/README.md](firmware/c/README.md). The
companion Android app lives in its own repository,
[wake-electronics/tinta-tap](https://github.com/wake-electronics/tinta-tap).

## Field results

A 4.2-inch unit running the Seatsurfing use case has been logged continuously in an office:

```
393 days, 5142 wake cycles on one set of three AAA cells
99.5 % day coverage; one outage in 393 days (66.6 h, external cause)
45-minute wake interval, median 2700.0 s over 13 months, no measurable drift
```

The 4.2-inch version is rated at about 5,000 wake cycles on three AAA cells, the 7.5-inch version
at about 10,000 on three AA cells. The unit above met that rating in the field and kept going.
Actual runtime follows the configured wake interval: a longer interval means fewer cycles per day
and a longer life on the same cells.

## License

This project contains both hardware and software components, licensed under separate terms:

- Everything in **/hardware** is licensed under the
  **CERN Open Hardware License v2 — Strongly Reciprocal (CERN-OHL-S-2.0)**.
- All original content in **/firmware** is licensed under the **Apache License, Version 2.0**.
  Bundled third-party components under `/firmware/c/third_party/` retain their upstream licenses —
  see [firmware/c/third_party/README.md](firmware/c/third_party/README.md).

See the LICENSE files in the respective directories for the full texts.

## Contributing

Issues and pull requests are welcome, particularly new use cases: one directory under
`firmware/c/` and one build flag, see [firmware/README.md](firmware/README.md).

Contact: [c0de@posteo.de](mailto:c0de@posteo.de)
