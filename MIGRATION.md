# Migration: Elegoo ESP32 devkit → FireBeetle 2 ESP32-C5 (DFR1222)

## Why
The Elegoo devkit idles at ~25mA even in deep sleep — the AMS1117 regulator,
CP2102 USB-serial chip, and power LED all draw continuously, upstream of the
ESP32's sleep domain. Result: ~9 days on a 10,000mAh power bank (rated 6,250mAh
at 5V after boost losses). The FireBeetle 2 is battery-first: onboard LiPo
charging, and a raw 3.7V cell feeds it directly with no boost/buck conversion
tax — so the full ~10,000mAh is usable.

**Status: all five displays verified working on the C5 bench setup.**

## Wiring

Shared by all five panels:

| Display pin | GPIO | Silkscreen |
|---|---|---|
| CLK | 23 | `23/SCK` |
| DIN (= MOSI) | 24 | `24/MO` |
| DC | 10 | `10/SCL` |
| RST | 9 | `9/SDA` |
| VCC | — | `3V3` (always-on rail) |
| GND | — | any `GND` |

Per panel:

| Display | Content | CS | CS silkscreen | BUSY | BUSY silkscreen |
|---|---|---|---|---|---|
| display1 | MM (minutes) | 8 | `8/D2` | 6 | `6/D12/LP_SDA` |
| display2 | HH (hours) | 5 | `5/A4/LP_TX` | 11 | `11/TX` |
| display3 | DD (day) | 4 | `4/A3/LP_RX` | 25 | `25/MI` |
| display4 | MMM (month) | 3 | `3/A2` | 7 | `7/D11/LP_SCL` |
| display5 | DDDE / weather | 2 | `2/A1` | 12 | `12/RX` |

**display5 (7.5") also has a PWR pin** — a power-enable input, not a supply.
Tie it to the always-on `3V3` alongside VCC. Leaving it unconnected means the
module never powers up.

Left unused: GPIO1 (battery ADC), GPIO15 (onboard LED), GPIO26/27/28
(boot-mode strapping), GPIO0 (3V3_C rail control).

### Notes on the pin choices
- **GPIO11/12 are free** because the C5 uses native USB CDC for Serial — UART0
  is never used. (This is also why the COM port re-enumerates every minute:
  deep sleep powers down the USB peripheral.)
- **GPIO25 (MISO) is free** because e-ink is write-only. `SPI.begin()` is called
  with MISO = -1 so the SPI driver doesn't claim it.
- **GPIO7 and 25 are strapping pins, but not boot-mode ones** (only 26/27/28 set
  boot mode). They're used as BUSY *inputs*, never driven, and are assigned to
  DD and MMM — the two panels that refresh least often, so any surprise is rare
  and low-impact.

## Gotchas that cost time (don't repeat these)

1. **`3V3_C` is a switched rail, default OFF.** DFRobot: *"IO0 controls 3.3V
   power output, default off, can be turned on with high level."* Powering a
   display from it without driving GPIO0 high means a completely dead panel with
   no error. Use the plain `3V3` pin.
2. **The boxed `3V3_C / GND / SCL / SDA` group** shares nets with `10/SCL` and
   `9/SDA` elsewhere on the board — it's a second access point, not extra pins.
3. **DIN on the display = MOSI.** Panel-side naming; there is no data-out line.

## Arduino IDE settings
- **Board:** FireBeetle 2 ESP32-C5, with a current **ESP32 core 3.x** (the C5 is
  RISC-V and recent — older cores don't list it).
- **Partition Scheme:** must have ≥2MB app. The sketch is ~1.7MB (WiFi/TLS stack
  plus ~440KB of bitmap PROGMEM). Default 1.25MB overflows. Use `Huge APP`,
  `Minimal SPIFFS`, or `No OTA`. If the options look sparse, check **Flash Size**
  first — setting it too low collapses the partition choices.
- **PSRAM: Disabled** — otherwise boot throws
  `MSPI Timing: Failed to allocate dummy cacheline for PSRAM memory barrier!`
- **USB CDC On Boot: Enabled** — otherwise `Serial` goes to UART0 (GPIO11/12),
  not the USB port, and you get no output at all.

### Serial debugging on native USB
Deep sleep drops the USB connection each cycle, and re-enumeration takes 1–2s —
by which time the sketch has already printed and gone back to sleep. To actually
read output, add a temporary startup delay:

```cpp
Serial.begin(115200);
delay(3000);   // let USB CDC enumerate — REMOVE for battery operation
```

3s awake per minute is a 5% duty cycle: fine on USB, ruinous on battery.
(Baud rate is irrelevant over native USB CDC — any value works.)

## Outstanding
- **`gpio_deep_sleep_hold_en()` / `_dis()` aren't exposed on the C5 core.** The
  RST hold via `gpio_hold_en(GPIO_NUM_9)` is still in place, but without the
  global deep-sleep-hold latch it may not persist through sleep. Find the C5
  equivalent, and confirm GPIO9 is hold-capable.
- **RTC clock source** — run once to see whether a 32.768kHz crystal is fitted:
  ```cpp
  #include "soc/rtc.h"
  Serial.printf("RTC slow clk: %d (0=internal RC, 1=ext 32k crystal)\n",
                rtc_clk_slow_freq_get());
  ```
  Affects drift between the 30-minute NTP syncs only — not correctness.
- **Wake-duration optimization.** Every wake currently inits and hibernates all
  five panels even when only MM changed. Initializing just the panels about to be
  drawn should cut awake time, which is where nearly all the battery goes
  (~3–5mA averaged active vs ~0.05mA asleep). No wiring impact.
- **Don't bother with switched display power.** Cutting panel VCC wipes the
  controller's frame RAM, forcing a full refresh every wake — more current and
  more visible flashing than the ~20–50µA it would save.

## What carried over unchanged
Deep-sleep architecture, hibernate logic, weather retry/stale handling, refresh
cadence, `RTC_DATA_ATTR` state, and `secrets.h` config are all chip-agnostic.
Only pin numbers, the RST hold GPIO, and the explicit `SPI.begin()` changed.

## Sources
- https://wiki.dfrobot.com/dfr1222/
- https://documentation.espressif.com/esp32-c5_datasheet_en.html
