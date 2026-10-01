# Wiring — ST7796 4.0" module → ESP32-P4-WIFI6-DEV-KIT

> **Every pin number here also lives in exactly one place in code:**
> [`firmware/main/bsp_pins.h`](../firmware/main/bsp_pins.h).
> If your bench wiring differs, change that file and nothing else.

---

## SD card: which slot?

**Use the P4's on-board TF slot (board callout #24, underside). Leave the display's SD slot empty
and leave display pin 14 (`SD_CS`) unconnected.**

| | Display SD slot | **P4 TF slot** |
|---|---|---|
| Bus | SPI, **shared with the LCD** | **Dedicated SDIO 3.0** |
| Width | 1-bit | **4-bit** |
| Pins used | MOSI/SCK/MISO (shared) + `SD_CS` | GPIO39–44, nothing else wants them |
| Contention with display | **Constant** | **None** |
| Costs you a header GPIO | Yes | No |

Reasons, in order of how much they'll hurt you:

1. **Bus contention.** The display's SD lines *are* the LCD's SPI lines — only the chip-select
   differs. A dashboard that redraws continuously while logging every detection would be
   serialising those two activities through one mutex forever.
2. **Clock whiplash.** The ST7796 wants a fast write-only clock (40–80 MHz). An SD card in SPI
   mode negotiates a slower clock and needs re-init. Sharing means reconfiguring the bus on every
   handoff — which is both slow and a classic source of SD corruption.
3. **Throughput.** 1-bit SPI versus 4-bit SDIO is roughly an order of magnitude. Detection logging
   is bursty; you want the writes to finish and get out of the way.
4. **It's free.** The P4 slot's pins are already committed to it. Using it costs you no header
   pins, and leaving `SD_CS` disconnected frees one more GPIO.

Your 32 GB card is SDHC → format **FAT32** (exFAT also works but FAT32 is the safer default).
Neither slot has an advantage on capacity, so this is purely about the bus.

---

## Pin map

Display header pin 1 is the **`VCC`** end. Note the silkscreen on the 2.54 mm block reads
top-to-bottom as pin 14 → pin 1, so count from the `VCC` end, not the top.

| Disp. pin | Signal | → P4 GPIO | Header pin | Notes |
|---:|---|---|---:|---|
| 1 | `VCC` | **5V** | **2** | 5 V, not 3V3 — backlight dims badly at 3.3 V |
| 2 | `GND` | **GND** | **6** | |
| 3 | `LCD_CS` | **GPIO21** | **11** | active low |
| 4 | `LCD_RST` | **GPIO20** | **13** | active low |
| 5 | `LCD_RS` | **GPIO22** | **12** | DC: high = data, low = command |
| 6 | `SDI (MOSI)` | **GPIO6** | **15** | SPI2 |
| 7 | `SCK` | **GPIO5** | **16** | SPI2 |
| 8 | `LED` | **GPIO3** | **19** | backlight, driven by LEDC PWM |
| 9 | `SDO (MISO)` | **GPIO4** | **18** | optional — ST7796 reads only |
| 10 | `CTP_SCL` | **GPIO8** | **5** | shared I2C bus |
| 11 | `CTP_RST` | **GPIO2** | **21** | active low |
| 12 | `CTP_SDA` | **GPIO7** | **3** | shared I2C bus |
| 13 | `CTP_INT` | **GPIO1** | **22** | goes low on touch |
| 14 | `SD_CS` | — | — | **leave unconnected** |

Twelve wires. Both 5 V pins (header 2 and 4) and any GND (6, 9, 14, 20, 25, 30, 34, 39) are fine.

### Why these pins

- `GPIO7`/`GPIO8` are the board's **existing I2C bus** (header pins 3/5) and already have pull-ups.
  The FT6336U sits at `0x38`; the on-board ES8311 codec is at `0x18`. No collision, so the touch
  controller just joins the bus.
- `GPIO1–6` and `GPIO20–22` are plain GPIOs on a contiguous run of the header, which keeps the
  ribbon tidy.
- The P4 routes SPI through the GPIO matrix, so any pin works for SPI2 — there's no "correct"
  hardware set to match.

### Pins deliberately avoided

| Header pin | Why |
|---|---|
| **32 (GPIO54)** | **C6 enable/reset.** Grounding it kills Wi-Fi and BLE with no obvious symptom. |
| 8, 10 (GPIO37/38) | Console UART — you want serial logs while debugging this. |

Already committed off-header: GPIO14–19 + 54 (C6 SDIO), GPIO39–44 (TF card).

---

## Checks before first boot

1. **Continuity from header pin 32 to the display: there must be none.** Verify this first.
2. `VCC` on 5 V, not 3V3.
3. Count from the `VCC` end of the display header, not the top of the silkscreen.
4. With the board powered and the card in, `ls /sdcard` over the console should enumerate.

---

## Spare pins

Unused after this build: GPIO0, 23, 24, 25, 26, 27, 32, 33, 36, 45, 46, 47, 48, 53 —
plus header 3V3 on pins 1/17.

A UART GPS module (for real wardriving fixes) fits naturally on GPIO23/GPIO24 with 3V3 and GND.
The logger already carries latitude/longitude/fix columns; they stay empty until a GPS is present.
