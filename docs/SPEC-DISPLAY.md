# Spec Sheet — 4.0" Capacitive SPI LCD Module (ST7796S)

**Source:** `DISPLAY/4.0inch Capacitive SPI Module ST7796 - LCD wiki.pdf` (lcdwiki.com)
**Vendor page:** <https://www.lcdwiki.com/4.0inch_Capacitive_SPI_Module_ST7796>

## Identity

| Field | Value |
|---|---|
| SKU (with touch) | **MSP4031** ← assumed yours (capacitive touch) |
| SKU (no touch) | MSP4030 |
| Silkscreen | `4.0" TFT SPI 480X320 V1.0  Capacitive Touch` |
| Weight (boxed) | MSP4031 106 g / MSP4030 85 g |

## LCD

| Parameter | Value |
|---|---|
| Panel size | 4.0 inch |
| Panel type | **TN** (not IPS — viewing angle is directional) |
| Resolution | **320 × RGB × 480** px |
| Driver IC | **ST7796S** |
| Interface | **4-wire SPI** (CS / RST / RS(DC) / MOSI / SCK / MISO) |
| Colors | 16.7 M (65 K / RGB565 over SPI) |
| Active area | 55.68 (W) × 83.52 (H) mm |
| Pixel pitch | 0.174 × 0.174 mm |
| Best viewing angle | **12 o'clock** (look at it from "below" — mount accordingly) |
| Brightness | 300 cd/m² typ. |
| Backlight | 8 × white LED, ~103 mA, 0.5 W |
| Operating temp | −20 … 60 °C |
| Storage temp | −30 … 70 °C |

## Touch

| Parameter | Value |
|---|---|
| Type | Capacitive (CTP) |
| Driver IC | **FT6336U** |
| Bus | **I²C**, 7-bit address **0x38** |
| Touch resolution | 320 × 480 |
| Visual area | 55.98 (W) × 83.82 (H) mm |
| Operating temp | −20 … 70 °C |

> FT6336U is a **2-point** controller. Design the UI for single-tap + swipe; do not rely on pinch/multi-gesture.

## Electrical

| Parameter | Value |
|---|---|
| Logic | On-board level shifter — **5 V and 3.3 V MCU safe** |
| VCC | 5.0 V **recommended**. At 3.3 V the backlight is visibly dim. |
| Backlight current | 103 mA |
| Power | 0.5 W |

> **Feed VCC from the P4 header's 5 V pin (pin 2 or 4), not 3V3.** The on-board shifter handles the P4's 3.3 V logic. Running VCC at 3.3 V costs you a lot of brightness for a device you'll read in a car.

## Mechanical

| Parameter | Value |
|---|---|
| TFT outline | 60.88 × 94.57 × 2.5 mm |
| Touch outline | 60.88 × 94.57 × 1.35 mm |
| Module outline (touch, incl. header) | 60.88 × 108.0 × 14.80 mm |
| Module outline (no touch, incl. header) | 60.88 × 108.0 × 12.95 mm |

## Connectors

| # | Connector | Notes |
|---|---|---|
| 1 | 14P header, 2.54 mm | Signal input. This is the one you'll use. |
| 2 | Micro SD (TF) slot | **Shares the LCD SPI bus** — see warning below |
| 3 | 14P FPC, 0.5 mm (P2) | Same signals as the header |

## 14-Pin Header Pinout

Silkscreen order on the P2 block, pin 1 → 14:

| Pin | Label | Function |
|---:|---|---|
| 1 | `VCC` | Power +. Use 5 V. |
| 2 | `GND` | Ground |
| 3 | `LCD_CS` | LCD chip select, **active low** |
| 4 | `LCD_RST` | LCD reset, **active low** |
| 5 | `LCD_RS` | Command/Data select. **High = data, low = command.** (a.k.a. DC) |
| 6 | `SDI (MOSI)` | SPI write data — *shared with SD card* |
| 7 | `SCK` | SPI clock — *shared with SD card* |
| 8 | `LED` | Backlight control. Leave floating = always on; drive with PWM to dim. |
| 9 | `SDO (MISO)` | SPI read data — *shared with SD card* |
| 10 | `CTP_SCL` | Touch I²C clock |
| 11 | `CTP_RST` | Touch reset, active low |
| 12 | `CTP_SDA` | Touch I²C data |
| 13 | `CTP_INT` | Touch interrupt — goes **low** on touch |
| 14 | `SD_CS` | SD card chip select, active low. **Leave unconnected** (see below). |

> On the physical 2.54 mm header the labels read **top-to-bottom as pin 14 → pin 1** (`SD_CS` at top, `VCC` at bottom). Count from the `VCC` end.

## ⚠️ The SD slot on this display shares the LCD SPI bus

Pins 6/7/9 (MOSI/SCK/MISO) are **common to the LCD and the SD card**; only the chip-selects differ. That means:

- Every SD write has to arbitrate against display drawing on the same bus.
- The ST7796 wants a fast write-only clock (40–80 MHz); an SD card in SPI mode needs a slower, negotiated clock and re-initialisation. You end up reconfiguring the bus constantly.
- SPI-mode SD is **1-bit** — roughly an order of magnitude slower than 4-bit SDIO.

**Decision: don't use this slot.** Leave pin 14 (`SD_CS`) unconnected and log to the P4's own TF slot. Full reasoning in [WIRING.md](WIRING.md#sd-card-which-slot).

## Reference documents

- [MSP4030/MSP4031 Specification](https://www.lcdwiki.com/res/MSP4030_MSP4031/MSP4030_MSP4031_Specification_EN_V1.0.pdf)
- [User manual](https://www.lcdwiki.com/res/MSP4030_MSP4031/4.0inch_SPI_Module_MSP4030_MSP4031_User_Manual_EN.pdf)
- [Module schematic](https://www.lcdwiki.com/res/MSP4030_MSP4031/4.0inch_SPI_Module_MSP4030_MSP4031_Schematic.pdf)
- [ST7796S datasheet](https://www.lcdwiki.com/res/MSP4030_MSP4031/ST7796S-Sitronix.pdf)
- [FT6336U datasheet](https://www.lcdwiki.com/res/MSP4030_MSP4031/DFT6336UDataSheetV1.1.pdf)
- [ST7796 init sequence](https://www.lcdwiki.com/res/MSP4030_MSP4031/ST7796_Init.txt)
