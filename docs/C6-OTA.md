# Flashing the ESP32-C6 co-processor

The C6 is the radio. If its ESP-Hosted slave firmware is missing, old, or a
different major version from the host side, Wi-Fi and BLE simply do not work —
usually with no obvious symptom beyond `esp_hosted_init failed` in the log.

The C6's UART is on **unrouted pads** on this board (callout #4), so wired
flashing means soldering. That is why OTA matters here.

---

## Current state of this build

| | |
|---|---|
| Component | `espressif/esp_hosted` **3.0.9**, referenced by `path:` |
| Located at | `C:/Users/atruett/esp/components/esp_hosted` |
| Why not the registry | 3.x **cannot be copied into `managed_components/`** on this machine — see [the path-length problem](#the-path-length-problem) |
| OTA in use | chunked `begin/write/end/activate`, streaming straight off the SD card |

**SD-card OTA is active.** Go to [Route A](#route-a--sd-card-ota-recommended).

`c6_ota.c` picks its implementation at compile time:

```c
#ifdef esp_hosted_slave_ota_begin
#  define C6_OTA_CHUNKED 1   /* 3.x: stream from the SD card */
#else
#  define C6_OTA_CHUNKED 0   /* 2.x: compile the SD path out, say so clearly */
#endif
```

So if the dependency is ever swapped back to a 2.x registry version, the build
still succeeds and `c6_ota_run()` returns `ESP_ERR_NOT_SUPPORTED` with a log
line pointing here — rather than failing mysteriously at runtime.

---

## The path-length problem

esp_hosted 3.x contains files like:

```
port/idf_components/esp_wifi_remote/esp_wifi_remote/components/esp_wifi_remote/
    test/smoke_test/components/esp_hosted/idf_v6.1/include/esp_hosted_mock.h
```

That is ~150 characters on its own. Added to this project's
`managed_components/espressif__esp_hosted/` prefix (112 chars, because the repo
lives under `Documents/ALT/github/...`) it exceeds Windows' 260-char `MAX_PATH`,
and the component manager dies while unpacking:

```
FileNotFoundError: [Errno 2] No such file or directory: '...esp_hosted_examples_common.h'
shutil.Error: [WinError 3] The system cannot find the path specified
```

A junction to a short path does **not** help — CMake resolves it back to the
real directory.

### The fix used here: reference the component in place

A `path:` dependency is **used where it sits** — the component manager never
copies it into `managed_components/`. So the deep tree only has to fit under a
short root, which it does:

```yaml
# firmware/main/idf_component.yml
espressif/esp_hosted:
  path: "C:/Users/atruett/esp/components/esp_hosted"
```

```
C:\Users\atruett\esp\components\esp_hosted\   42 chars
  + deepest internal file                    ~155 chars
  = 197                                      well under 260
```

Needs no administrator rights and no moving the repo. `esp_wifi_remote` resolves
to the same local copy automatically, so there is only ever one esp_hosted in
the build.

To re-create it on another machine:

```powershell
# let the component manager fetch it once (the download and unpack to its
# own cache succeed -- only the copy into the project fails)
$env:IDF_COMPONENT_CACHE_PATH = 'C:\tmp\cc'
# then lift it out to a short, permanent home:
robocopy C:\tmp\cc\service_*\espressif__esp_hosted_3.0.9_* `
         C:\Users\atruett\esp\components\esp_hosted /E
```

and point `path:` at wherever you put it.

### Alternative: enable long paths (needs admin)

If you ever have administrator rights on the machine, this is the tidier fix and
is Espressif's documented recommendation for ESP-IDF on Windows:

```powershell
New-ItemProperty -Path 'HKLM:\SYSTEM\CurrentControlSet\Control\FileSystem' `
  -Name LongPathsEnabled -Value 1 -PropertyType DWORD -Force
git config --system core.longpaths true
```

Then the `path:` override can be replaced with a plain
`espressif/esp_hosted: "^3.0.9"`. Not required — the current setup works
without it.

---

## ⚠ The chicken-and-egg: RPC versions must match exactly

Before relying on OTA, understand its one hard limit. From esp_hosted's own
`docs/architecture.md`, on the host↔co-processor handshake:

> **RPC-version negotiation: the RPC version must match. This path is a strict
> match — a mismatch aborts the firmware**, so host and co-processor builds
> must agree.

OTA rides on the RPC control plane. So:

**If the C6's existing firmware is too old to complete the RPC handshake with
our 3.x host, the link never comes up — and OTA cannot rescue it,** because OTA
needs that same link. There is no bootstrap path over SDIO.

This matters because the board arrives with whatever slave firmware Waveshare
flashed, which may be an older esp_hosted.

**How to tell:** flash the P4 and watch the console. You want to see
`esp_hosted` bring the link up and `net_link` report a co-processor version:

```
I (xxxx) net_link: starting esp_hosted link to C6...
I (xxxx) net_link: co-processor esp_hosted v3.0.9
I (xxxx) net_link: radio link up
```

If instead you get `esp_hosted_init failed` or the handshake aborts, the C6
needs [Route C](#route-c--wired-recovery) once — wire the jumpers to its UART
pads and flash `c6-firmware/build/wardrive_c6_slave.bin` directly. After that
one wired flash, host and co-processor are on the same version and every
future update can go over SDIO.

---

## Route A — the P4 pushes it over SDIO

Needs esp_hosted 3.x on the host side, and a C6 already running slave firmware
new enough to complete the RPC handshake (see the warning above).

Two places the image can come from. The SD card wins if both are present.

### A1 — embedded in the P4's flash (no SD card)

`c6-firmware/` builds the slave image, and the P4's `CMakeLists.txt` writes it
into a dedicated `c6fw` partition as part of `idf.py flash`:

```powershell
cd c6-firmware
idf.py set-target esp32c6
idf.py build
cd ../firmware
idf.py -p COMxx flash          # flashes P4 app AND the c6fw partition
```

On boot the P4 reads the partition, compares it against the version+size it
last successfully pushed (kept in NVS), and updates the C6 only if they differ.
So it flashes exactly once per change, not on every boot.

This is the path that needs **nothing** beyond the USB cable — no SD card, no
jumpers, no soldering.

### A2 — from the SD card

Useful for trying a different image without rebuilding the P4.

1. Use `c6-firmware/build/wardrive_c6_slave.bin`, or any slave image built for
   the **ESP32-C6 over SDIO** (prebuilt ones ship with the
   [esp-hosted-mcu releases](https://github.com/espressif/esp-hosted-mcu/releases)).

2. Put two files in the card root:

   ```
   /c6_slave.bin        the image
   /c6_update.flag      an empty file -- the "please do it" marker
   ```

3. Boot the board. On startup it will:
   - validate the image header and read its version string,
   - confirm the hosted link is alive,
   - erase the C6's inactive OTA partition,
   - stream the image in 1400-byte chunks (tens of seconds; the header shows
     `OTA nn%`),
   - verify, activate, and let the C6 reboot into it,
   - **delete `c6_update.flag`** so the next boot does not repeat it.

The flag file is what makes this safe to leave the image on the card
permanently — without the flag, the firmware only logs the image version and
moves on.

Scanners are paused for the duration; the radio is gone while the C6 restarts.

---

## Route B — URL OTA (what 2.x offers)

`esp_hosted_slave_ota(const char *image_url)` downloads over HTTP from the
P4. It needs the P4 **joined to a Wi-Fi network**, which this firmware
deliberately never does (it stays in STA mode purely as a scan vantage point,
and only ever scans passively).

If you want it anyway, join a network before calling it and host the image on
any HTTP server. It is a one-line call — but it means your survey tool starts
transmitting, which rather defeats the purpose.

---

## Route C — wired recovery

The fallback when the C6 is blank or its firmware is too broken to accept OTA.

1. Pull **C6_IO9 low** to force the C6 into download mode.
2. Put the P4 into download mode too (hold BOOT, tap RST).
3. Flash through the C6 UART pads — `C6_U0RXD` / `C6_U0TXD` (callout #4).

This needs soldering or a well-held test clip. It is the only route that works
on a C6 that has never been flashed.

---

## Enabling Zigbee

The Zigbee screen reports `RADIO UNAVAILABLE` today. Two honest routes exist,
both with real costs.

### Route 1 — 802.15.4 RCP on this C6 — costs you the Wi-Fi screen

`esp_hosted` 3.x does support this, and ships a working
`examples/zigbee/thermostat` pair. The architecture, from its README:

```
                   ESP-Hosted bus (SDIO)
  [ P4 host ] --- RPC: RCP control ------> [ ESP-Hosted CP ]   control stops here
             \
              \    dedicated UART
               +-- 802.15.4 spinel (data) -> [ 802.15.4 RCP ] -- RF -> Zigbee device
```

Two roles on the **same C6**, not two chips. But note the parenthesis in that
README: *"Both are the same ESP32-C6 **(Wi-Fi off)**"*.

**That is the killer.** This is not duty-cycle sharing — the RCP role is built
with Wi-Fi disabled. Turning Zigbee on here turns the **Wi-Fi screen off**.
For a survey tool whose main job is counting APs, that is a bad trade.

It also still needs the soldering: *"the 802.15.4 spinel data plane always
rides a separate dedicated UART"*, and that UART is on the C6's unrouted pads.

So Route 1 buys you Zigbee at the price of Wi-Fi, plus a soldering iron.

### Route 2 — a second radio (recommended)

Hang a separate 802.15.4 part — an **ESP32-H2** is the natural choice — off a
spare UART and let it do nothing but 802.15.4. The P4 has plenty of free GPIOs
and the header carries 3V3 and GND (see [WIRING.md](WIRING.md#spare-pins)).

Strictly better for this project:

- Wi-Fi, BLE **and** Zigbee all live at once, because nothing is shared.
- No soldering to fragile pads.
- No risk of destabilising the hosted link your other three screens depend on.
- The H2 is a few dollars.

### Wiring it in

Either way, the integration surface is one function. `scan_zigbee.c` already
defines it:

```c
void zigbee_report(const uint8_t ext_addr[8], uint16_t panid,
                   uint8_t channel, int8_t rssi, uint8_t lqi);
```

Call that for each beacon your 802.15.4 source produces and the Zigbee screen,
the combined totals and the SD log all start working with no other changes.

---

## Note on Matter

Matter needs **none** of this. The Matter screen already works on stock
firmware, because Matter devices are discoverable over the two radios you
already have:

- **BLE** — commissioning adverts under service UUID `0xFFF6`, carrying
  discriminator, vendor ID and product ID.
- **Wi-Fi mDNS** — `_matterc._udp` (commissionable) and `_matter._tcp`
  (operational).

A Thread-only Matter device still beacons over BLE during commissioning, so it
shows up there too.
