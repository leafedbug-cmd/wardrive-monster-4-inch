# Spec Sheet — Waveshare ESP32-P4-WIFI6-DEV-KIT (Rev 1.2)

**Source:** `ESP32-P4-WIFI6-DEV-KIT REV1.2/…pdf` + [Waveshare wiki](https://www.waveshare.com/wiki/ESP32-P4-WIFI6-DEV-KIT)
**Your kit:** **Basic Kit** — board + speaker. No DSI LCD, no camera.
**Part No.** ESP32-P4-WIFI6-DEV-KIT · **SKU** 32054 · **Brand** Waveshare

> Because you have the Basic Kit, the MIPI-DSI connector is unused and the ST7796 goes on the
> **40-pin GPIO header over SPI**. That is what this project assumes throughout.

---

## 1. Board at a glance

| Item | Detail |
|---|---|
| Host MCU | **ESP32-P4** (32 MB PSRAM in package) |
| Radio co-processor | **ESP32-C6** — Wi-Fi 6 + BT 5 / BLE, over **SDIO 3.0** |
| Flash | 16 MB NOR (on the P4) |
| Storage | **microSD (TF) slot, SDIO 3.0, 4-bit** — on the underside |
| Networking | 100M RJ45 Ethernet; reserved PoE header |
| USB | USB-OTG 2.0 HS x2 (A ports, HOST/DEVICE jumper) |
| Audio | ES8311 codec, on-board mic, 3.5 mm jack, MX1.25 2P speaker header (8 ohm 2 W) |
| Expansion | 40-pin GPIO header (2x20), 28 programmable GPIOs |
| Camera | MIPI-CSI 2-lane (unused on Basic Kit) |
| Display | MIPI-DSI 2-lane (unused on Basic Kit) |
| Other | RTC battery header (rechargeable only), BOOT + RST buttons |
| Programming | Type-C **USB** port and Type-C **UART** port |

---

## 2. ESP32-P4 (the host)

| Parameter | Value |
|---|---|
| Cores | Dual-core 32-bit **RISC-V** HP, up to **400 MHz**, AI extensions, single-precision FPU |
| LP core | Single-core RISC-V, up to 40 MHz |
| HP ROM | 128 KB |
| LP ROM | 16 KB |
| HP L2MEM | 768 KB |
| LP SRAM | 32 KB |
| TCM | 8 KB zero-wait |
| PSRAM | **32 MB in-package** |
| Flash | 16 MB NOR (board) |
| GPIO | 55 programmable (28 brought to the header) |
| **Wi-Fi / Bluetooth** | **None.** The P4 has no radio at all. |
| Media | JPEG codec, Pixel Processing Accelerator, ISP, **H.264 encoder** (1080p30) |
| Interfaces | MIPI-CSI, MIPI-DSI, USB-OTG 2.0 HS, Ethernet MAC, **SDIO Host 3.0**, SPI, I2S, I2C, I3C, LED PWM, MCPWM, RMT, ADC, UART, TWAI |
| Security | Secure Boot, Flash Encryption, crypto accelerators, TRNG, Digital Signature Peripheral, Key Management Unit, privilege separation |

**The single most important fact:** the P4 is a pure applications processor. *Every* packet in this
project — every Wi-Fi beacon, every BLE advertisement — is captured by the C6 and shipped to the
P4 over SDIO. Scan performance is bounded by the C6 and that bus, not by the P4.

---

## 3. ESP32-C6 (the radio)

| Parameter | Value |
|---|---|
| Core | Single-core 32-bit RISC-V HP @ 160 MHz + LP RISC-V @ 20 MHz |
| SRAM | 512 KB HP + 16 KB LP |
| ROM | 320 KB |
| **Wi-Fi** | **802.11ax (Wi-Fi 6)** 2.4 GHz, 20 MHz BW; also 802.11b/g/n. OFDMA, MU-MIMO (DL), **TWT** |
| **Bluetooth** | **Bluetooth 5.3 LE** — 2 Mbps PHY, Long Range (Coded PHY), Extended Advertising, Mesh |
| **802.15.4** | Present in silicon — **Thread 1.3 / Zigbee 3.0** capable |
| Antenna | On-board SMD antenna |
| Link to P4 | **SDIO 3.0, 4-bit** |

### One radio, three protocols

Wi-Fi, BLE and 802.15.4 all share **a single 2.4 GHz front end**. The C6 time-slices between them;
it cannot genuinely listen on all three at once. Concurrent Wi-Fi + BLE works (the coexistence
arbiter interleaves them) at the cost of duty cycle on each. Adding 802.15.4 means a third claimant
on the same radio. **Plan for time-division, not parallelism.** This is the root cause of the
Zigbee limitation in section 6.

---

## 4. P4 <-> C6 interconnect (ESP-Hosted)

The C6 runs **ESP-Hosted slave** firmware; the P4 runs ESP-Hosted host + `esp_wifi_remote`.
To application code, `esp_wifi_*` and NimBLE look local — they are RPC'd over SDIO.

| Signal | P4 GPIO |
|---|---|
| SDIO CLK | **GPIO18** |
| SDIO CMD | **GPIO19** |
| SDIO D0 | **GPIO14** |
| SDIO D1 | **GPIO15** |
| SDIO D2 | **GPIO16** |
| SDIO D3 | **GPIO17** |
| **C6 reset / EN** | **GPIO54** |

These are the ESP-Hosted defaults for the ESP32-P4, so no pin configuration is required.

> **WARNING: GPIO54 is also exposed on 40-pin header pin 32.** Pulling that pin resets your radio.
> **Never connect anything to header pin 32.** This is the easiest way to silently break this build.

### C6 UART pads

The board exposes **ESP32-C6 UART pads** (board callout #4) for wired recovery flashing.
They are **pads, not a header, and are not routed to the P4**. Consequences:

- Wired C6 flashing means soldering, or careful pogo/test-clip contact.
- Anything needing a dedicated P4<->C6 UART (e.g. Thread/Zigbee spinel RCP) needs that solder work.

Recovery if OTA ever fails: pull **C6_IO9 low** to force C6 download mode, put the P4 in download
mode too, and flash via `C6_U0RXD` / `C6_U0TXD`.

---

## 5. microSD (TF) slot — SDIO 3.0

| Signal | P4 GPIO |
|---|---|
| CLK | **GPIO43** |
| CMD | **GPIO44** |
| D0 | **GPIO39** |
| D1 | **GPIO40** |
| D2 | **GPIO41** |
| D3 | **GPIO42** |

4-bit SDMMC, powered by the on-chip LDO, `SDMMC_SLOT_CONFIG_DEFAULT()` with internal pull-ups.
Dedicated pins — **no contention with the display or the C6.** The 32 GB card goes here.

---

## 6. What this hardware can and cannot scan

| Screen | Radio path | Status |
|---|---|---|
| **Wi-Fi** | C6 Wi-Fi scan -> `esp_wifi_remote` -> P4 | Fully supported |
| **BLE** | C6 BLE controller -> HCI over SDIO -> NimBLE on P4 | Supported, with caveats |
| **Matter** | mDNS `_matterc._udp` / `_matter._tcp` over Wi-Fi **plus** BLE service UUID **0xFFF6** | Needs no 802.15.4 |
| **Zigbee** | Requires raw 802.15.4 | **Not available on stock firmware** |

### BLE caveats (known upstream issues)

- **Scan stalls after ~90 s.** Advertising reports stop being delivered over SDIO
  ([esp-hosted-mcu#180](https://github.com/espressif/esp-hosted-mcu/issues/180)).
  The firmware runs a watchdog that tears down and restarts the scan on a timer.
- **Use passive scan.** Active scanning (which transmits SCAN_REQ) is markedly worse — the bug
  appears to be in the C6's TX path over SDIO. Passive is also the correct choice for wardriving:
  you observe without transmitting.
- Set `CONFIG_BT_NIMBLE_STATIC_TO_DYNAMIC=n`.

### Why Zigbee is blocked

Zigbee needs raw 802.15.4. `esp_hosted` 3.x *does* support it — as an 802.15.4 **RCP** whose
spinel data plane rides a **dedicated UART**, in addition to the SDIO bus. Two problems on this
board:

1. **That UART is on unrouted C6 pads** (section 4), so it needs soldering.
2. **The RCP role is built with Wi-Fi off.** Espressif's own example states the two roles are
   *"the same ESP32-C6 (Wi-Fi off)"*. This is not duty-cycle sharing — enabling Zigbee on this
   chip **disables the Wi-Fi screen**.

For a tool whose main job is counting APs, that is the wrong trade. A separate ESP32-H2 on a
spare UART gets all four protocols running at once.

The Zigbee screen is therefore **built and wired up, but reports `RADIO UNAVAILABLE`** rather than
inventing data. Upgrade paths are documented in [C6-OTA.md](C6-OTA.md).

---

## 7. Board callouts (from the vendor PDF)

| # | Item | # | Item |
|---:|---|---:|---|
| 1 | ESP32-P4 (32 MB PSRAM) | 13 | MIPI-CSI camera (2-lane) |
| 2 | ESP32-C6 (SDIO 3.0) | 14 | MIPI-DSI display (2-lane) |
| 3 | 16 MB NOR flash | 15 | I2C interface |
| 4 | **ESP32-C6 UART pads** | 16 | I3C interface |
| 5 | PoE module header | 17 | Type-C UART (power/flash/debug) |
| 6 | 40-pin GPIO header | 18 | Type-C USB (power/flash) |
| 7 | 100M RJ45 Ethernet | 19 | RTC battery header |
| 8 | USB OTG HOST/DEVICE jumper | 20 | PWR LED |
| 9 | USB-A ports (OTG 2.0 HS) | 21 | C6 SMD antenna |
| 10 | Speaker header (MX1.25 2P) | 22 | BOOT button |
| 11 | On-board microphone | 23 | RST button |
| 12 | 3.5 mm audio jack | 24 | **TF card slot (SDIO 3.0)** |

---

## 8. 40-pin GPIO header

Raspberry-Pi-style 2x20. Odd pins left column, even pins right.

| Pin | Signal | | Pin | Signal |
|---:|---|---|---:|---|
| 1 | 3V3 | | 2 | 5V |
| 3 | **SDA / GPIO7** | | 4 | 5V |
| 5 | **SCL / GPIO8** | | 6 | GND |
| 7 | GPIO23 | | 8 | TXD / GPIO37 |
| 9 | GND | | 10 | RXD / GPIO38 |
| 11 | GPIO21 | | 12 | GPIO22 |
| 13 | GPIO20 | | 14 | GND |
| 15 | GPIO6 | | 16 | GPIO5 |
| 17 | 3V3 | | 18 | GPIO4 |
| 19 | GPIO3 | | 20 | GND |
| 21 | GPIO2 | | 22 | GPIO1 |
| 23 | GPIO0 | | 24 | GPIO36 |
| 25 | GND | | 26 | GPIO32 |
| 27 | GPIO24 | | 28 | GPIO25 |
| 29 | GPIO33 | | 30 | GND |
| 31 | GPIO26 | | 32 | **GPIO54 — C6 ENABLE, DO NOT USE** |
| 33 | GPIO48 | | 34 | GND |
| 35 | GPIO53 | | 36 | GPIO46 |
| 37 | GPIO47 | | 38 | GPIO27 |
| 39 | GND | | 40 | GPIO45 |

### Pins to leave alone

| Pin(s) | Why |
|---|---|
| **32 (GPIO54)** | C6 enable/reset. Touching it kills Wi-Fi + BLE. |
| 8, 10 (GPIO37/38) | Console UART — you want these for logs |
| 3, 5 (GPIO7/8) | Shared I2C: ES8311 codec `0x18`, camera SCCB. Safe to *share* with FT6336U at `0x38`. |

Not on the header and already committed: GPIO14-19 + 54 (C6), GPIO39-44 (TF card).

---

## 9. Power

- 5 V via either Type-C port, or PoE via the reserved header.
- Backlight alone draws ~103 mA; budget for the C6 transmitting plus SD writes on top.
- For in-vehicle use, feed it from a supply good for **at least 1.5 A at 5 V** — brownouts during
  SD writes corrupt log files.

---

## Sources

- [Waveshare product page](https://www.waveshare.com/esp32-p4-wifi6-dev-kit.htm)
- [Waveshare wiki](https://www.waveshare.com/wiki/ESP32-P4-WIFI6-DEV-KIT)
- [Board schematic/datasheet](https://files.waveshare.com/wiki/ESP32-P4-WIFI6-DEV-KIT/ESP32-P4-WIFI6-DEV-KIT-datasheet.pdf)
- [Espressif ESP32-P4](https://www.espressif.com/en/products/socs/esp32-p4)
- [esp-hosted-mcu](https://github.com/espressif/esp-hosted-mcu)
