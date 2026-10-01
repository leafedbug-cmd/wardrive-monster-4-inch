# Wardrive Monster — 4"

A Wi-Fi / BLE / Matter survey dashboard on a Waveshare **ESP32-P4-WIFI6-DEV-KIT**
(Basic Kit) driving a 4.0" **ST7796S** SPI panel, logging every detection to the
P4's own microSD slot.

```
ESP32-P4   application, display, storage, UI
ESP32-C6   the radio -- Wi-Fi 6 + BLE, over SDIO via ESP-Hosted
ST7796S    4.0" 480x320 SPI panel on the 40-pin header
microSD    the P4's SDIO slot (NOT the one on the display -- see below)
```

## Five screens

Swipe between them, or let it auto-cycle every 12 s.

| | Screen | Source | Status |
|---|---|---|---|
| 0 | **Combined** | all of the below | working |
| 1 | **Wi-Fi** | passive all-channel sweep via `esp_wifi_remote` | working |
| 2 | **BLE** | passive NimBLE observer against the C6 controller | working |
| 3 | **Matter** | mDNS `_matterc._udp` / `_matter._tcp` + BLE UUID `0xFFF6` | working |
| 4 | **Zigbee** | raw 802.15.4 | **unavailable on stock firmware** |

The Zigbee screen says so plainly rather than inventing rows. The C6 has the
802.15.4 radio, but the ESP-Hosted slave does not expose it and the UART an RCP
link would need is on unrouted pads. Full reasoning and two upgrade paths are in
[docs/C6-OTA.md](docs/C6-OTA.md#enabling-zigbee).

## SD card: use the slot on the P4

The display has an SD slot too, but it **shares the LCD's SPI bus** — only the
chip-select differs. The P4's slot is dedicated 4-bit SDIO on GPIO39–44 with no
contention and roughly an order of magnitude more throughput. Leave display pin
14 (`SD_CS`) unconnected. [Full reasoning](docs/WIRING.md#sd-card-which-slot).

Format the 32 GB card as **FAT32**. Logs land in `/sdcard/wardrive/sess-NNNN.csv`,
one file per boot, with a session counter kept in NVS so files never collide.

## Two wiring traps

1. **Header pin 32 is GPIO54 — the C6's enable line.** Connect anything to it
   and Wi-Fi and BLE die with no obvious symptom. Leave it alone.
2. **Feed the display VCC from 5 V**, not 3V3. The on-board level shifter
   handles the P4's 3.3 V logic; at 3.3 V the backlight is badly dim.

## Build

Needs ESP-IDF **v5.5.2 or newer** — built and verified against **v5.5.5**.

> Not optional on this board. These kits ship with ESP32-P4 silicon at
> **revision v3.1**, and IDF up to 5.5.1 caps the P4 at v1.99, so `idf.py flash`
> refuses outright:
>
> ```
> A fatal error occurred: bootloader/bootloader.bin requires chip revision
> in range [v0.1 - v1.99] (this chip is revision v3.1)
> ```
>
> v5.5.2 raised the ceiling to v3.99. Don't work around it by editing
> `ESP32P4_REV_MAX_FULL` in an older IDF: newer versions carry real
> rev-3-conditional code (`ESP32P4_SELECTS_REV_LESS_V3`), so bumping the number
> alone can produce a subtly wrong binary.

```powershell
cd firmware
idf.py set-target esp32p4
idf.py build
idf.py -p COMx flash monitor
```

`esp_hosted` 3.0.9 is referenced by `path:` from
`C:/Users/atruett/esp/components/esp_hosted` rather than downloaded, because its
internal tree is too deep to copy into this repo under Windows' 260-char limit.
[Why, and how to re-create it elsewhere](docs/C6-OTA.md#the-path-length-problem).

## Flashing the C6

The C6's UART is on bare pads, so there's no cable to plug into it. Instead the
P4 pushes firmware over the SDIO link they already share. Its source firmware
lives in [`c6-firmware/`](c6-firmware/) and is built from the **same**
`esp_hosted` tree as the host — if host and co-processor are built from
different versions, the RPC wire format between them can disagree.

Two ways to deliver it, both needing no soldering and no network:

**Embedded in the P4's flash** (no SD card needed) — the C6 image is written to
a `c6fw` partition as part of `idf.py flash`, and the P4 pushes it to the C6 on
the next boot if it differs from what was last flashed.

**From the SD card** — drop `c6_slave.bin` and an empty `c6_update.flag` in the
card root. Takes precedence over the embedded copy, so it's the way to try a
different image without rebuilding.

Either way it takes ~15 s and shows `OTA nn%` in the header. [Details](docs/C6-OTA.md).

> OTA travels over the hosted RPC link, so it needs the C6 to already be running
> *some* working slave firmware. A blank C6 has to be recovered over its UART
> pads once — [Route C](docs/C6-OTA.md#route-c--wired-recovery).

## Documentation

| | |
|---|---|
| [docs/SPEC-DISPLAY.md](docs/SPEC-DISPLAY.md) | ST7796S / FT6336U module spec sheet |
| [docs/SPEC-ESP32-P4-C6.md](docs/SPEC-ESP32-P4-C6.md) | P4 + C6 board and chip spec sheet |
| [docs/WIRING.md](docs/WIRING.md) | pin map, SD card decision, pre-boot checks |
| [docs/C6-OTA.md](docs/C6-OTA.md) | flashing the C6, and enabling Zigbee |

Vendor PDFs the spec sheets were built from are in `DISPLAY/` and
`ESP32-P4-WIFI6-DEV-KIT REV1.2/`.

## Layout

```
firmware/main/
  bsp_pins.h      every board-dependent number, in one file
  app_main.c      bring-up order
  display.c       ST7796 + FT6336 + LVGL
  ui.c            the five screens
  store.c         dedup'd detection table (PSRAM)
  sdlog.c         queued CSV logger
  net_link.c      ESP-Hosted / C6 bring-up
  c6_ota.c        C6 firmware update over SDIO
  scan_*.c        the four detection sources
```

Change wiring in `bsp_pins.h` and nothing else.
