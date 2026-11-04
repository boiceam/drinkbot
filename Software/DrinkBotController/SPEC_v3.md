# DrinkBotController — Software Specification

| | |
|---|---|
| **Version** | 3.0 (approved baseline) |
| **Supersedes** | `SPEC_v2.md`, `SPEC.md` (v1) |
| **Target hardware** | Seeed Studio XIAO ESP32-S3 |
| **Companion device** | DrinkBotPump (RP2040), firmware ≥ 0.4.0 (see §6) |
| **Date** | 2026-11-03 |

Requirement keywords: **shall** means mandatory, **may** means optional. Every recommendation in v2 has been approved and is now a **shall**. Items tagged **[A-n]** are approved assumptions (§1.2), and **[D-n]** are design decisions (§1.1).

### Revision history

| Version | Changes |
|---|---|
| 1 | Initial spec (`SPEC.md`). |
| 2 | Restructured. Added pump protocol details, pump firmware v0.3.0 changes, config schema, state model, WebSocket protocol, error catalog and acceptance tests. |
| 3 | All v2 recommendations and assumptions approved. PF-9, PF-10 and the AP captive DNS are now required. Added a per-pump `autoPrimeSec` config value, plus **Prime All** and **Purge All** (§9.6.2). |
| 3.1 | Manual prime/purge runs until stopped (at most 120 s) with a Stop button, one pump at a time; starting another pump stops the running one first (§9.6.1). Pump firmware 0.4.0: `dispense <ms>` takes a run time, and `smilli`/`gmilli` are removed (PF-11, D-6). |

---

## Table of Contents

1. [Decisions & Assumptions](#1-decisions--assumptions)
2. [System Overview](#2-system-overview)
3. [Hardware](#3-hardware)
4. [Software Architecture](#4-software-architecture)
5. [Pump Communication](#5-pump-communication)
6. [Required DrinkBotPump Firmware Changes (v0.3.0, v0.4.0)](#6-required-drinkbotpump-firmware-changes-v030-v040)
7. [Configuration](#7-configuration)
8. [System State Model](#8-system-state-model)
9. [System Behaviors](#9-system-behaviors)
10. [Physical UI: Buttons & Controller LEDs](#10-physical-ui-buttons--controller-leds)
11. [Pump LED Behavior](#11-pump-led-behavior)
12. [Logging](#12-logging)
13. [Web Server & HTTP Endpoints](#13-web-server--http-endpoints)
14. [WebSocket Protocol](#14-websocket-protocol)
15. [Web Application UI](#15-web-application-ui)
16. [Simulator Mode](#16-simulator-mode)
17. [Error Catalog](#17-error-catalog)
18. [Timing & Limits Constants](#18-timing--limits-constants)
19. [Acceptance Tests](#19-acceptance-tests)
20. [Glossary](#20-glossary)

---

## 1. Decisions & Assumptions

There are no open items.

### 1.1 Decisions

| # | Decision |
|---|---|
| D-1 | **Pin map:** the XIAO D-labels are the wiring truth. Button 7 is on D9 (GPIO 8) and NeoPixel data on D10 (GPIO 9). v1's GPIO column had these swapped and is corrected in §3.2. |
| D-2 | **USB power:** the bridge pump is **self-powered**. The USB link carries data only (§3.1). |
| D-3 | **Pump idle LEDs:** **off** when the level is OK. Only low, empty, dispense and prime/purge states light them (§11). |
| D-4 | **Clearing latched errors without a browser:** hold the `estop` button for **3 s** (§10.1). |
| D-5 | **Pump firmware:** may be changed. v0.4.0 is required (§6, PF-11). |
| D-6 | **Volume to pump time:** the controller converts each pour to a run time (`ml × msPerMl`) and sends `dispense <ms>` (§9.5). The pump stores no flow rate (PF-11). *(Rev 3.1; was `smilli` then `dispense <ml>`.)* |
| D-7 | **Scale tare:** each scale is tared with its empty container on it, so weight = liquid only. |
| D-8 | **Bridge pump:** it dispenses too and has its own ID, so it is addressed directly over USB. |
| D-9 | **Toolchain:** PlatformIO + Arduino-ESP32 core (§4.1). |
| D-10 | **Missing pump at boot:** degraded error state (§17). |
| D-11 | **Pour check:** compare against the weight change and log a warning only (§9.5). |
| D-12 | **Extras in scope:** pump maintenance UI, config import/export, OTA, mDNS. |
| D-13 | **All v2 recommendations approved:** pump firmware safety cap (PF-9), `sid` range check (PF-10), and the captive DNS in AP mode (§9.2) are required. |
| D-14 | **Auto prime:** each pump has an `autoPrimeSec` config value. **Prime All** primes each pump in turn for its `autoPrimeSec`. **Purge All** purges each pump in turn for `1.5 × autoPrimeSec` (§9.6.2). |

### 1.2 Approved assumptions

| ID | Assumption |
|---|---|
| A-1 | The NeoPixel count is **9** (v1's web section said "10 UI LEDs", which is treated as a typo). |
| A-2 | "slow yellow red state" in v1 means **slow red pulse** for an empty pump. |
| A-3 | Liquid thresholds work like this: **Low** and **Indicated Empty** are warnings that change pump LEDs. **Actually Empty** is the unpumpable reserve. A dispense is allowed only if `level − requested ≥ actuallyEmptyMl`. |
| A-4 | Before the first pour, the controller checks **all** of a recipe's ingredients. If any fail, nothing is dispensed, so a half-made drink never happens. |
| A-5 | Ingredients with the same `order` value are poured in their listed order. Pours are always sequential, never parallel. |
| A-6 | The active recipe and drink volume are **not** persisted. At boot they come from `defaults` in the config. |
| A-7 | Retired in v3 (replaced by D-3). |
| A-8 | A button press gives a green flash on its LED for 250 ms, then the LED returns to its role state. Actions fire on press (falling edge), not release. |
| A-9 | E-Stop aborts the current operation and stops all pumps. It does **not** latch, so the system returns to Ready (or Error, if a fault exists). |
| A-10 | In AP fallback mode the controller does not keep retrying station mode. A reboot retries. |
| A-11 | Max 16 pumps. Pump IDs 1–254 are valid; 0 is the factory default and invalid; 255 is broadcast. |
| A-12 | Wi-Fi passwords are returned to the web UI in plain text (v1: "no security"). The UI masks them by default. |
| A-13 | Up to 4 simultaneous WebSocket clients. |
| A-14 | Recipes are referenced by a stable `id` (so renaming doesn't break button bindings). Ingredients match pump liquids by **case-insensitive name**. |

---

## 2. System Overview

The DrinkBotController is a XIAO ESP32-S3 that dispenses mixed drinks. It does this by telling a set of DrinkBotPump nodes, one at a time, to pour set volumes in a set order. It has a physical UI (7 buttons and 9 NeoPixels) and a self-hosted web app reached over Wi-Fi.

```
                      Wi-Fi (STA or AP "DrinkBot")
   Browser  <------------------------------------------->  XIAO ESP32-S3 (DrinkBotController)
   (web app, WebSocket JSON)                                 |  7 buttons, 9 NeoPixels
                                                             |  UART0 debug log (D6/D7)
                                                             | USB-C (host mode, CDC-ACM)
                                                             v
                                                   DrinkBotPump #A  (the "bridge pump")
                                                   - dispenses its own liquid (direct)
                                                   - relays to bus with `bridge <id> <cmd>`
                                                             | RS485 shared bus, 115200 baud
                         +-------------------+---------------+---------------+
                         v                   v                               v
                  DrinkBotPump #B     DrinkBotPump #C        ...      DrinkBotPump #N
```

### 2.1 Key facts carried from the pump firmware

- The bridge pump is a normal dispensing pump with its own ID. The controller learns that ID at boot with a direct `gid` and talks to it **directly** over USB. Every other pump is reached through `bridge`.
- Each pump drives 20 NeoPixels, reads an HX711 load cell, and runs a reversible pump on a timer.
- The pump has no completion event for a pour, so the controller has to track pour time itself. v0.3.0 adds `status` to confirm it (§6).

---

## 3. Hardware

### 3.1 MCU

- Seeed XIAO ESP32-S3: dual-core 240 MHz, 8 MB flash, 8 MB PSRAM.
- Native USB-C is set to **USB host** mode, so it **cannot** carry the serial console or normal USB flashing at runtime. See §4.5.
- **Power (D-2):**
  - The bridge pump has its own supply, so the USB link carries data only. The XIAO gets its own 5 V on its 5V pin, or 3.3 V on its 3V3 pin.
  - The RP2040 USB stack (TinyUSB) forces VBUS-detect, so it enumerates without VBUS.
  - **Hardware caution:** the XIAO's 5V pin connects to the USB-C VBUS. If the XIAO is fed at 5V and the pump board also drives its USB VBUS from its own rail, the two supplies are tied together. Make sure one of them doesn't drive VBUS: leave VBUS unconnected in the cable/adapter, or make sure the pump's VBUS is input-only. Alternatively, use a common supply for both.
  - Both boards must share ground (USB GND).

### 3.2 Pin mapping (corrected — see D-1)

| XIAO Pin | ESP32-S3 GPIO | Function | Config |
|---|---|---|---|
| D0  | GPIO 1  | Button 1 | Input, internal pull-up, active low |
| D1  | GPIO 2  | Button 2 | Input, internal pull-up, active low |
| D3  | GPIO 4  | Button 3 | Input, internal pull-up, active low |
| D4  | GPIO 5  | Button 4 | Input, internal pull-up, active low |
| D5  | GPIO 6  | Button 5 | Input, internal pull-up, active low |
| D8  | GPIO 7  | Button 6 | Input, internal pull-up, active low |
| D9  | GPIO 8  | Button 7 | Input, internal pull-up, active low |
| D10 | GPIO 9  | NeoPixel data out (9 × WS2812/GRB, 800 kHz) | Output |
| D6  | GPIO 43 | Debug UART TX (log output, 115200 8N1) | UART0 TX |
| D7  | GPIO 44 | Debug UART RX (unused / reserved for console) | UART0 RX |
| D2  | GPIO 3  | Unused (strapping pin, leave unconnected) | — |
| USB-C | GPIO 19/20 | USB host to bridge pump | USB OTG |

### 3.3 Buttons

- Software debounce: an edge counts only after the level has been stable for **30 ms**.
- Each button fires its action once per press, on the **press** edge [A-8]. Holding a button doesn't repeat it.
- The one long-press behavior is on an `estop` button. After the E-Stop has fired, keeping it held for `LONG_PRESS_MS` (3000) also clears latched errors (§10.1).
- Buttons are polled at ≥ 100 Hz, or read by GPIO interrupt with deferred debounce.

### 3.4 NeoPixels

- 9 pixels in one chain, GRB order. Index mapping is in §10.2.
- The global brightness cap is `ui.ledBrightness` (default 64/255), to limit glare and current.
- Refresh runs at 50 Hz (20 ms), which matches the pump firmware's LED task rate.

---

## 4. Software Architecture

### 4.1 Toolchain

| Item | Choice |
|---|---|
| Build system | PlatformIO |
| Platform | `pioarduino` (Arduino-ESP32 core **3.x** on ESP-IDF 5.x). Needed for the USB-host CDC-ACM driver. |
| Board | `seeed_xiao_esp32s3` |
| USB-on-boot | `ARDUINO_USB_MODE=0`, `ARDUINO_USB_CDC_ON_BOOT=0` (frees the USB peripheral for host mode) |
| Partition table | `default_8MB.csv` (app0 / app1 for OTA, plus a LittleFS data partition) |
| Filesystem | LittleFS. Holds **config only** (§7.1). |

### 4.2 Libraries

| Purpose | Library |
|---|---|
| USB host CDC-ACM | ESP-IDF `usb_host` + `espressif/usb_host_cdc_acm` component |
| Web server + WebSocket | `ESPAsyncWebServer` + `AsyncTCP` (maintained fork: mathieucarbou) |
| JSON | `ArduinoJson` v7 |
| NeoPixels | `Adafruit NeoPixel` (RMT-backed on ESP32) |
| mDNS | `ESPmDNS` (core) |
| OTA | `Update` (core) |
| Filesystem | `LittleFS` (core) |

### 4.3 Modules

| Module | Responsibility |
|---|---|
| `Log` | Leveled logger. Fans out to UART0, a ring buffer and WebSocket clients (§12). |
| `ConfigStore` | Loads, validates, defaults and saves config atomically. Handles import/export (§7). |
| `PumpLink` | USB host CDC-ACM transport and line framing. Handles direct and bridged transactions, CRC and timeouts (§5). Interface is `IPumpLink`, with real and simulated versions (§16). |
| `PumpManager` | Discovery, the per-pump model (presence, version, weight, level, LED state), background polling, and LED state commands (§9.3, §11). |
| `Dispenser` | Recipe → volume math, pre-checks, the step sequencer, verification and abort (§9.5). |
| `UiPanel` | Button debounce, action dispatch, controller LED rendering (§10). |
| `NetManager` | Wi-Fi STA attempts, AP fallback, mDNS (§9.2). |
| `WebServer` | Static assets, HTTP endpoints, OTA, WebSocket hub (§13, §14). |
| `SystemState` | The global state machine (§8) and change notifications. |

### 4.4 Concurrency model

- One **command queue** (FreeRTOS queue) feeds the **App task**. Button actions, WebSocket requests and internal timers all post commands to it, so behavior has one owner and races are avoided.
- **PumpLink** runs one transaction at a time, guarded by a mutex. Background polling only happens when no dispense, prime, purge or maintenance transaction is in progress.
- Web callbacks (AsyncTCP task) **shall not** block or call PumpLink directly. They queue commands and return.
- Outbound WebSocket broadcasts go through a queue drained by the App task (or `textAll`), rate-limited so slow clients can't stall the system.
- Suggested tasks: `usb_host_lib` (IDF), `PumpLink RX`, `App` (state + dispenser), `UI` (buttons + LEDs, 20 ms tick), and the Async web tasks.

### 4.5 Flashing & debug

- **First flash:** hold BOOT while plugging the XIAO into a PC. The ROM bootloader enumerates as a USB device regardless of the app's host mode.
- **Updates after that:** OTA from the web UI (§13).
- **Debug log:** UART0 on D6/D7 at 115200. Use a USB-UART adapter.

### 4.6 Web assets

- The web app is plain HTML, CSS and JavaScript, with **no build step and no external/CDN references**.
- A PlatformIO pre-build script gzips the files in `web/` and **embeds them in the firmware image** as byte arrays, served with `Content-Encoding: gzip`.
- Embedding means an OTA firmware update also updates the UI, and a filesystem image upload can't wipe `config.json`.
- Target total gzipped size: < 150 KB.

---

## 5. Pump Communication

### 5.1 USB transport

- The controller opens the first CDC-ACM interface it enumerates. It does not filter by VID/PID; the device it finds is expected to be RP2040 `arduino-pico`.
- Line coding is 115200 8N1. This is nominal for USB CDC but set anyway.
- After opening, the controller **shall assert DTR and RTS**. The RP2040 TinyUSB CDC stack may drop TX until DTR is asserted.
- Detection: if no CDC device enumerates within `USB_DETECT_TIMEOUT_MS` (3000) after USB host start, the controller enters **Simulator mode** (§16).
- Disconnect at runtime: abort any operation and enter Error `E_USB_LOST`. When a device re-enumerates, re-run discovery (§9.3).

### 5.2 Line framing (controller side)

- TX: ASCII command + `\n`. Max 63 chars. The bridged inner command is ≤ 63 chars, ≤ 8 space-separated tokens, and the first token is ≤ 15 chars.
- RX: accumulate bytes and split on `\n`, stripping `\r`. Max line 256 bytes; longer lines are discarded with a warning.
- Lines that don't start with `R,`, `P,` or `$[` (e.g. pump debug logs) are **ignored**, logged at VERBOSE.

### 5.3 Direct transaction (bridge pump)

```
TX:  weight\n
RX:  P,WEIGHT,123.456000\n      (0..n data lines)
RX:  R,OK\n                     (terminal: R,OK | R,ERR | R,INVLD)
```

The transaction completes on the first `R,` line. Timeout: `DIRECT_TIMEOUT_MS` (500).

### 5.4 Bridged transaction (all other pumps), pump FW ≥ 0.3.0

```
TX:  bridge 3 weight\n
RX:  $[P,WEIGHT,123.456000|9A1B2C3D]\n
RX:  $[R,OK|0F1E2D3C]\n            (terminal when inner payload starts with "R,")
 or
RX:  R,TIMEOUT\n                   (bridge gave up after kBridgeTimeoutMs = 2000)
```

- The controller **shall verify each `$[...]` CRC** and treat a bad CRC as a failed transaction.
- Controller timeout: `BRIDGE_TIMEOUT_MS` (2500). This must be longer than the pump's 2000 ms.
- CRC algorithm: CRC-32/ISO-HDLC (reflected, poly `0xEDB88320`, init `0xFFFFFFFF`, xorout `0xFFFFFFFF`), formatted as 8 uppercase hex digits.
  - Request CRC covers `*[<id>|<cmd>|`.
  - Response CRC covers `$[<payload>|`.
  - The bridge pump builds request packets; the controller only verifies responses.
- Payloads cannot contain `|`, `]` or newline.

> **v0.2.0 compatibility note:** in 0.2.0 the bridge first writes a local `R,OK` right after sending. It then forwards **only the first** response packet; for `weight` that is `P,WEIGHT` without the `R,OK`. Nothing is sent on timeout. If 0.2.0 support is ever needed, the controller must treat the first `$[` line as the whole response. It must also tell the local `R,OK` apart from the bridged result, and rely only on its own timeout. 0.3.0 removes these quirks (§6).

### 5.5 Retry policy

| Command class | Retries | Rationale |
|---|---|---|
| Read-only (`version`, `gid`, `weight`, `status`) | 2 | Safe to repeat. |
| LED commands | 1 | Harmless to repeat. |
| Motion (`dispense`, `prime`, `purge`) | **0 automatic.** On timeout, send `status`: if the pump is running, treat the command as successful; if idle, retry once. | Prevents a double pour. |
| `stop` | 3 | Must get through. Broadcast is also used (§9.7). |
| `tare`, `cal`, `sid`, `scfg` | 0 | User-initiated; report the failure. |

### 5.6 Commands used by the controller

| Command | Use |
|---|---|
| `version` | Discovery; check the minimum version (≥ 0.4.0). |
| `gid` | Learn the bridge pump's ID (direct only). |
| `weight` | Level checks, verification, background polling. |
| `dispense <ms> [ledMode]` | Pour: run forward for `<ms>` (= `ml × msPerMl`, 1–120000). |
| `prime <ms>` / `purge <ms>` | Prime / purge. |
| `stop` *(new)* | Abort the pump. |
| `status` *(new)* | Confirm a pour is done; motion retry logic. |
| `ledcolor`, `ledmode`, `ledfade`, `ledchase`, `ledtime` | Pump LED states (§11). |
| `tare`, `cal <g>`, `sid <id>` + `scfg` | Maintenance (§15.7). |

---

## 6. Required DrinkBotPump Firmware Changes (v0.3.0, v0.4.0)

Gaps found in pump firmware 0.2.0 that this design depends on:

| # | Change | Reason / current behavior |
|---|---|---|
| PF-1 | **Add `stop`**: immediately de-energize the pump, clear the dispense LED timer, and reply `R,OK`. | 0.2.0 has no stop. (`prime 0` works only as an accident of the timer logic.) Needed for E-Stop. |
| PF-2 | **Add `status`**: reply `P,STATUS,<IDLE\|FWD\|REV>,<remainingMs>` then `R,OK`. | The controller can't otherwise confirm a pour has finished, or whether a motion command took effect. |
| PF-3 | **Fix `dispense <ml>`**: parse `ml` as a float (`atof`) and compute duration in `uint32_t`. Reject `ml ≤ 0` or `ml > 1000` with `R,ERR`. | 0.2.0 stores `ml` in `uint8_t`, so `dispense 300` silently pours 44 ml and fractions are truncated. |
| PF-4 | **Busy rejection**: `dispense`, `prime` and `purge` reply `R,ERR` if the pump is already running. | 0.2.0 silently restarts the timer. |
| PF-5 | **Bridge forwarding**: send no local `R,OK` after transmitting. Forward **every** `$[...]` response line until one whose payload starts with `R,`. On timeout, write `R,TIMEOUT\n`. | 0.2.0 drops the second response packet (the `R,OK` after `P,...`) and reports nothing on timeout. |
| PF-6 | **Broadcast address 255**: every pump runs the command and **does not reply**. If `bridge 255 <cmd>` is sent, the bridge pump also runs it locally. Allowed only for `stop` and LED commands. | Gives E-Stop one fast transmission to all pumps. |
| PF-7 | **Version** `0.3.0`. | Superseded by PF-11: the controller now requires `0.4.0`. |
| PF-8 | Production builds keep the USB `Serial` free of log output (Debug Port = Disabled). | Keeps the line protocol clean (the controller ignores stray lines anyway). |
| PF-9 | **Hardware safety cap**: any motion lasts at most 120 s. | Limits a runaway if the controller crashes mid-prime. |
| PF-10 | **`sid` range check**: reject 0 and 255 with `R,ERR`. | Reserved values (A-11). |
| PF-11 *(0.4.0)* | **`dispense <ms> [ledMode]`** takes a run time in ms (1–120000, PF-9) instead of a volume. `smilli`, `gmilli` and the stored ms/ml are removed; the other EEPROM fields keep their layout. Version `0.4.0`, which the controller now requires. Supersedes PF-3's ml parsing. | The controller owns the calibration (`msPerMl` in config), so the pump needs no flow-rate state. An older pump would read the run time as millilitres, so it must be refused. |

---

## 7. Configuration

### 7.1 Storage

- File: `/config.json` on LittleFS, UTF-8, ≤ 32 KB.
- **Atomic save**: write `/config.tmp`, verify it parses, rename the current file to `/config.bak`, then rename `/config.tmp` to `/config.json`.
- **Load order**: try `/config.json`, then `/config.bak`, then the built-in factory default. If a fallback was used, log at ERROR and raise `E_CONFIG` (non-blocking).
- A `schemaVersion` integer allows migrations. Unknown keys are kept on save.

### 7.2 Schema

```jsonc
{
  "schemaVersion": 2,

  "device": {
    "hostname": "drinkbot",            // mDNS name -> http://drinkbot.local
    "apSsid": "DrinkBot"               // open AP fallback SSID
  },

  "wifi": {
    "networks": [                      // tried in listed order
      { "ssid": "HomeWiFi", "password": "secret" }
    ]
  },

  "levels": {
    "lowMl": 150,                      // <= this: pump LED slow yellow pulse
    "emptyMl": 50,                     // <= this: pump LED slow red pulse
    "actualEmptyMl": 20                // dispensing may never take level below this
  },

  "dispense": {
    "verifyTolerancePct": 15,          // warn if |actual-target| > max(pct, minMl)
    "verifyToleranceMinMl": 5,
    "settleMs": 1500,                  // wait after pump stops before reading weight
    "minVolumeMl": 10,
    "maxVolumeMl": 1000
  },

  "defaults": {
    "volumeMl": 250,                   // active drink size at boot
    "recipeId": "margarita",           // active recipe at boot ("" = none)
    "primeSec": 5,                     // auto prime for a pump added in the web UI (and v1 migration)
    "purgeSec": 10                     // unused since rev 3.1; kept so existing configs stay valid
  },

  "ui": {
    "ledBrightness": 64                // controller NeoPixel cap 0-255
  },

  "pumps": [
    {
      "pumpId": 1,                     // 1-254, unique; the pump's bus address
      "liquid": "Tequila",             // unique (case-insensitive); "" = unloaded
      "msPerMl": 95,                   // 1-65535, controller converts ml to dispense run time
      "densityGPerMl": 0.95,           // grams per ml; level_ml = weight_g / density
      "color": [255, 160, 0],          // RGB used for this liquid's LEDs
      "autoPrimeSec": 4.0              // Prime All runs this long; Purge All runs 1.5x (0 = skip)
    }
  ],

  "recipes": [
    {
      "id": "margarita",               // stable slug, [a-z0-9-], unique
      "name": "Margarita",
      "ingredients": [
        { "liquid": "Tequila",     "parts": 2, "order": 1 },
        { "liquid": "Triple Sec",  "parts": 1, "order": 2 },
        { "liquid": "Lime Juice",  "parts": 1, "order": 3 }
      ]
    }
  ],

  "buttons": [                         // exactly 7 entries, index 0 = Button 1
    { "action": "setRecipe", "recipeId": "margarita" },
    { "action": "setRecipe", "recipeId": "paloma" },
    { "action": "setVolume", "volumeMl": 150 },
    { "action": "setVolume", "volumeMl": 250 },
    { "action": "setVolume", "volumeMl": 350 },
    { "action": "dispense" },
    { "action": "estop" }
  ]
}
```

Button `action` values: `none`, `setRecipe` (+`recipeId`), `setVolume` (+`volumeMl`), `dispense`, `estop`.

### 7.3 Validation rules

When validation fails, the save is rejected and an error naming each failing field path is returned. The saved file is unchanged.

- `pumps`: ≤ 16 entries. `pumpId` integer 1–254 and unique. `liquid` unique if non-empty. `msPerMl` 1–65535. `densityGPerMl` 0.5–2.0. `color` is 3 ints 0–255. `autoPrimeSec` 0–60 with 0.1 s resolution; 0 leaves the pump out of Prime All and Purge All. The longest Purge All run (1.5 × 60 = 90 s) is under the pump's 120 s cap (PF-9).
- `levels`: `0 ≤ actualEmptyMl < emptyMl < lowMl`.
- `recipes`: `id` unique, matches `^[a-z0-9-]{1,32}$`. `name` 1–32 chars. 1–16 ingredients. `parts` > 0 (decimal allowed). `order` integer ≥ 0.
- A recipe ingredient whose `liquid` isn't loaded on any pump is **allowed** (it may be loaded later). The recipe is then shown as *unavailable*.
- `buttons`: exactly 7. A referenced `recipeId` must exist. `volumeMl` must be within `[minVolumeMl, maxVolumeMl]`.
- `wifi.networks`: ≤ 8. `ssid` 1–32 bytes. `password` empty (open network) or 8–63 chars.
- Strings are trimmed. Names ≤ 32 chars.

### 7.4 Factory default

The factory default has empty `pumps`, `recipes` and `wifi.networks`, all buttons set to `none`, and the threshold and default values shown above. With no networks, Wi-Fi goes straight to AP mode.

### 7.5 Apply semantics

- Saving the config applies it right away, except for `wifi`, `device.hostname` and `device.apSsid`, which take effect at the next reboot. The UI says so.
- Saving is rejected while a dispense, prime or purge (including Prime All / Purge All) is in progress.

### 7.6 Schema migration

`schemaVersion` is now **2**. Loading a version 1 config adds `autoPrimeSec` to each pump, set to `defaults.primeSec`, then saves the result as version 2 and logs an INFO line.
- A saved `pumps` change triggers a rediscovery (§9.3).

---

## 8. System State Model

### 8.1 Primary states

| State | Meaning | Status LED |
|---|---|---|
| `BOOTING` | Init, Wi-Fi and pump discovery in progress | Solid white |
| `READY` | Idle, can dispense | Solid green |
| `DISPENSING` | Recipe pour in progress | Blink yellow |
| `MANUAL` | Prime, purge, Prime All, Purge All or a maintenance command running | Blink yellow |
| `ERROR` | One or more active errors (§17) | Blink red |

- `ERROR` sits on top of the others. If the active errors are *degraded* ones only (e.g. a missing pump), dispensing recipes that don't need the faulty pump is still allowed. The status LED alternates: blink red while in error, blink yellow while dispensing.
- Errors are cleared by the condition going away (e.g. a rescan finds the pump), or by the **Clear Errors** command for latched errors.

### 8.2 Transitions

```
BOOTING --discovery done, no errors--> READY
BOOTING --discovery done, errors-----> ERROR (degraded)
READY/ERROR(degraded) --dispense ok precheck--> DISPENSING --done/abort--> READY|ERROR
READY/ERROR(degraded) --prime/purge/primeAll/purgeAll/maint--> MANUAL --done/abort--> READY|ERROR
any --E-Stop--> stop all --> READY|ERROR
any --USB lost--> ERROR (E_USB_LOST)
```

### 8.3 Runtime (non-persisted) state

`activeRecipeId`, `activeVolumeMl`, `wifiMode` (`CONNECTING|STA|AP`), `ssid`, `ip`, `rssi`, `simulator` (bool), `errors[]`, `dispenseProgress`, `bulkProgress`, and per-pump `{present, fwVersion, weightG, levelMl, levelState, activity, led}`.

---

## 9. System Behaviors

### 9.1 Boot sequence

1. Start UART0 logging. Log the banner: firmware version, build date, reset reason.
2. Init the NeoPixels. Status LED solid white, Wi-Fi LED blinking green.
3. Mount LittleFS (format if mounting fails, and log ERROR). Load config (§7.1).
4. Start the USB host. Wait up to 3 s for a CDC device. If none, use simulator mode.
5. Start Wi-Fi (§9.2) **in parallel** with pump discovery. Discovery doesn't wait for Wi-Fi.
6. Pump discovery (§9.3), then set every pump's LED state (§11).
7. Start the web server and WebSocket as soon as there is an IP (STA or AP).
8. Enter `READY`, or `ERROR` if errors are active.

### 9.2 Wi-Fi

- Try each configured network in order, splitting the time among them, for **20 s total**. Each try is about `20 s / n` but at least 6 s, so a long list may not reach every entry.
- On success: STA mode, Wi-Fi LED solid green, start mDNS `<hostname>.local` with an `_http._tcp` service, and log the IP and RSSI.
- On failure: start an **open** AP with SSID `device.apSsid` (default `DrinkBot`), IP `192.168.4.1/24`, and DHCP. Wi-Fi LED solid yellow. mDNS also runs in AP mode.
  - Run a captive DNS server that resolves every name to 192.168.4.1, so phones open the UI automatically.
- STA connection lost at runtime: Wi-Fi LED goes back to blinking green while auto-reconnect runs. AP fallback is **not** re-triggered [A-10].

### 9.3 Pump discovery & rediscovery

1. Direct `version`. If it fails, raise `E_BRIDGE_UNRESPONSIVE` (blocking). Direct `gid` returns `bridgeId`.
2. For each configured pump: if `pumpId == bridgeId`, use the direct path; otherwise use bridged. Send `version`.
   - No reply: mark the pump `present=false` and raise `E_PUMP_MISSING(pumpId)` (degraded).
   - Version < 0.4.0: raise `E_PUMP_FW(pumpId)` (degraded; that pump can't dispense).
3. Read the initial `weight` of every present pump.
4. If `bridgeId` isn't in the configured pump list, log WARNING. The bridge pump still works as a relay.
5. Send `ledtime 0` before setting idle LEDs. This cancels the pump's 5 s boot-animation timer, which would otherwise turn the LEDs off.
6. Rediscovery is run by the Rescan command, by a `pumps` config save, or by USB re-enumeration. It clears `E_PUMP_MISSING` and `E_PUMP_FW` for pumps that now answer.

### 9.4 Background polling

- While idle (READY/ERROR), poll `weight` on one pump every `POLL_INTERVAL_MS / pumpCount`, so each pump is read about every 3 s.
- Update `levelMl = max(0, weightG / densityGPerMl)` and `levelState` (`OK`, `LOW`, `EMPTY`).
- When a pump's `levelState` changes, update its LEDs (§11) and broadcast `pumps`.
- Three consecutive poll failures raise `E_PUMP_MISSING` for that pump. A later success clears it.

### 9.5 Dispense algorithm

**Inputs:** `recipeId`, `volumeMl` (from the web request, or from active state for the button).

**Pre-checks** (all must pass, or nothing is poured; the failure reason is logged and reported):
1. The state allows a dispense (not DISPENSING or MANUAL, and no blocking error).
2. The recipe exists and `volumeMl` is in range.
3. Compute `ml_i = volumeMl × parts_i / Σparts`, rounded to 0.1 ml.
4. For each ingredient: a pump with a matching liquid exists, is present, and is on firmware ≥ 0.4.0. Each pour's run time (`ml × msPerMl`) is at most 120 s (PF-9).
5. Refresh `weight` for each pump involved, then check `levelMl − ml_i ≥ actualEmptyMl`. If a liquid appears twice in a recipe, sum its amounts.

**Sequence** (sort by `order` ascending; ties keep list order):

```
for each step i:
    broadcast dispenseProgress(step i, STARTING)
    w0 = weight(pump)
    expectedMs = round(msPerMl * ml_i)
    ledtime 0 ; ledcolor <liquid color> ; ledchase 50 4
    dispense(pump, expectedMs, 3)           // run time in ms; ledMode 3 = loop chase while pumping
    wait expectedMs + 300 ms (abortable, polling status every 250 ms)
    poll status until IDLE, max expectedMs + DISPENSE_OVERRUN_MS (5000)
        -> overrun: stop(pump), abort with E_DISPENSE_TIMEOUT
    wait settleMs
    w1 = weight(pump); actualMl = (w0 - w1) / density
    if |actualMl - ml_i| > max(ml_i*tolPct/100, tolMinMl): log WARNING (continue)
    restore pump idle LED state (§11)
    broadcast dispenseProgress(step i, DONE, actualMl)
broadcast dispenseProgress(COMPLETE, totals)
```

- **Abort triggers:** E-Stop, any pump transaction failure during the sequence, USB loss.
- **On abort:** `stop` the active pump (and broadcast stop), restore the LEDs, log which steps finished, and broadcast `dispenseProgress(ABORTED, reason)`. Leftover steps are **not** resumed.
- Changing the active recipe or volume during a dispense is allowed and affects the **next** dispense only.

### 9.6 Prime & purge

#### 9.6.1 Single pump

- Prime: `prime 120000` (forward). Purge: `purge 120000` (reverse). A manual run has no duration field: it runs for the pump's longest allowed run (`MANUAL_RUN_MS` = 120 s, PF-9) unless it is stopped first.
- **Stop:** `pumpStop {pumpId}` sends `stop` to the running pump; once it is acknowledged the pump's idle state is restored and the run reports `DONE` (reason `stopped`) then `COMPLETE`.
- **One pump at a time**, in state MANUAL. A prime or purge request for any pump while a manual run is active is **not** rejected: the running pump is stopped first, and the new run starts only after that `stop` is acknowledged. If the stop fails, `E_PUMP_COMM` is raised and the new run is not started. This also applies to the same pump in the other direction.
- Prime/purge requests during a dispense, Prime All, Purge All or maintenance command are rejected with `BUSY`; `pumpStop` during those is rejected with `BUSY` too (use E-Stop). E-Stop aborts a manual run.
- While running, the pump's LEDs show its liquid color in mode 4 (bounce). After it finishes, the idle state is restored.
- Prime/purge **skip** level checks, but a WARNING is logged if the pump is EMPTY.
- Completion is confirmed the same way as a dispense: wait the run time + 300 ms, then poll `status` until IDLE, for at most `DISPENSE_OVERRUN_MS` more.

#### 9.6.2 Prime All & Purge All (D-14)

These run one pump at a time, in sequence, using each pump's `autoPrimeSec`:

| Operation | Pump command | Duration per pump |
|---|---|---|
| **Prime All** | `prime <ms>` (forward) | `autoPrimeSec × 1000` ms |
| **Purge All** | `purge <ms>` (reverse) | `round(1.5 × autoPrimeSec × 1000)` ms |

**Which pumps:**
- Every configured pump with `autoPrimeSec > 0`, in ascending `pumpId` order.
- Pumps that are missing or on firmware < 0.4.0 are **skipped**, with a WARN log line and a `SKIPPED` progress entry. The rest of the sequence continues.
- Pumps with `autoPrimeSec = 0` are left out and not listed.
- If no pumps qualify, the request is rejected with `PRECHECK_FAILED`.

**Sequence:**

```
broadcast bulkProgress(op, STARTING, steps = qualifying pump count)
for each qualifying pump p (ascending pumpId):
    if p not present / FW too old: broadcast bulkProgress(step, SKIPPED); continue
    broadcast bulkProgress(step, RUNNING, durationMs)
    ledtime 0 ; ledcolor <liquid color> ; ledchase 50 4 ; ledmode 4 255
    prime|purge(p, durationMs)
    wait durationMs + 300 ms (abortable), then poll status until IDLE,
        max DISPENSE_OVERRUN_MS more -> overrun: stop(p), abort with E_DISPENSE_TIMEOUT
    restore p's idle LED state (§11)
    broadcast bulkProgress(step, DONE)
    wait BULK_GAP_MS (500) before the next pump
broadcast bulkProgress(op, COMPLETE)
```

**Rules:**
- The system is in state MANUAL for the whole sequence. Dispense, single prime/purge, config saves and other manual commands are rejected with `BUSY` until it finishes.
- **Abort triggers** are the same as for a dispense: E-Stop, a pump transaction failure (which also raises latched `E_PUMP_COMM`), or USB loss. On abort the active pump is stopped, LEDs are restored, `bulkProgress(ABORTED, reason)` is sent, and the remaining pumps are not run.
- Level checks are skipped. A WARN is logged for each pump that is EMPTY.
- Both operations are started from the web UI only. The web app asks for confirmation first (§15.3).
- Log lines: INFO at start (operation, pump count, total estimated time), at each pump (ID, liquid, duration) and at the end.

**Estimated total time** (shown in the confirm dialog) = Σ durations + (n − 1) × `BULK_GAP_MS` + n × 300 ms.

### 9.7 E-Stop

E-Stop comes from a button, the web header button, or the WS `estop` command. It runs **ahead of** everything else in the command queue.

1. Cancel the running sequence or manual operation.
2. Send `bridge 255 stop` (broadcast, PF-6), then send direct `stop` to the bridge pump.
3. Send an addressed `stop` to every present pump, with retries (§5.5), and collect any failures.
4. Log at WARNING `E-STOP activated (source=<button|web>)`. Any pump that didn't acknowledge raises `E_STOP_UNCONFIRMED(pumpId)` (latched; needs Clear Errors).
5. Restore pump LEDs and return to READY/ERROR [A-9].

### 9.8 Reboot

Reached from the web UI with a confirm dialog. Rejected while DISPENSING unless `force: true`, in which case the controller E-Stops first. It sends an ack, waits 500 ms, then calls `ESP.restart()`.

### 9.9 Volume & recipe selection

- `setVolume` sets `activeVolumeMl`. The web volume field and the volume buttons are two views of **the same** state.
- `setRecipe` sets `activeRecipeId`. The web recipe selector and the recipe buttons are synchronized the same way.
- Every change broadcasts `state`.

---

## 10. Physical UI: Buttons & Controller LEDs

### 10.1 Button actions

| Action | Effect |
|---|---|
| `setRecipe` | Set the active recipe. Its LED goes red; other recipe-button LEDs go off. |
| `setVolume` | Set the active volume. Its LED goes red; other volume-button LEDs go off. Matching is by value, so if the active volume equals a button's `volumeMl` (including when set from the web), that LED is red. |
| `dispense` | Dispense the active recipe at the active volume (§9.5). If the pre-check fails, flash this button's LED red 3× (fast, 150 ms) and log why. |
| `estop` | E-Stop fires on press (§9.7). **Holding it ≥ 3 s** also runs Clear Errors (D-4), the same as the web `clearErrors`. The button LED fast-blinks green 3× to confirm, and INFO `BTN - Latched errors cleared by long press` is logged. Non-latched errors (missing pump, etc.) stay until their condition clears. |
| `none` | Logged only. |

All presses: green feedback flash for 250 ms [A-8], and a log line at INFO `BTN - Button <n> pressed (<action>)`.

### 10.2 LED map & states

| LED | Function | States |
|---|---|---|
| 1–7 | Button 1–7 | **Green 250 ms** on press. Then by role: `setRecipe` / `setVolume` red if active, else off. `dispense` blinks yellow while DISPENSING, else off. `estop` off. `none` off. |
| 8 | Status | BOOTING solid white · READY solid green · DISPENSING/MANUAL blink yellow · ERROR blink red (an active dispense under degraded error alternates yellow/red). |
| 9 | Wi-Fi | Connecting/searching blink green · STA connected solid green · AP mode solid yellow. |

- "Blink" means 1 Hz, 50 % duty. "Fast blink" means 150 ms on / 150 ms off.
- Colors: green `(0,255,0)`, red `(255,0,0)`, yellow `(255,180,0)`, white `(255,255,255)`. All are scaled by `ui.ledBrightness`.
- The UiPanel keeps a model of all 9 LEDs `{r,g,b,mode}` and broadcasts it to the web app on change (§14).

---

## 11. Pump LED Behavior

The controller tracks the last LED commands sent to each pump. That is the "remote LED state" shown in the web UI.

| Pump condition | Commands sent | Appearance |
|---|---|---|
| Idle, level OK (D-3) | `ledtime 0` · `ledmode 1 0` | Off |
| Idle, LOW (`level ≤ lowMl`) | `ledtime 0` · `ledcolor 255 180 0` · `ledfade 1500 500 1500 500` · `ledmode 2 255` | Slow yellow pulse |
| Idle, EMPTY (`level ≤ emptyMl`) | `ledtime 0` · `ledcolor 255 0 0` · `ledfade 1500 500 1500 500` · `ledmode 2 255` | Slow red pulse |
| Dispensing | `ledcolor <liquid rgb>` · `ledchase 50 4` · `dispense <ml> 3` | Liquid-color loop chase |
| Prime/Purge (incl. Prime All / Purge All) | `ledcolor <liquid rgb>` · `ledchase 50 4` · `ledmode 4 255` · prime/purge | Liquid-color bounce |
| Missing / unloaded | (none) | Shown as unknown in the UI |
| LED test (maintenance) | Cycle R, G, B solid for 1 s each, then restore | — |

After **every** dispense, prime or purge, the pump's idle state is **re-sent**. The pump's `ledtime` expiry turns the strip off, so without this the LEDs would stay dark.

---

## 12. Logging

### 12.1 Levels & format

- Levels: `ERROR`, `WARN`, `INFO`, `DEBUG`, `VERBOSE`. The UART level and web level are set separately (defaults: UART = DEBUG, web = INFO). The web level can be changed from the UI at runtime and isn't persisted.
- Line format:

  ```
  [  123456] INFO  DISP - Step 2/3: Triple Sec 62.5 ml on pump 4
  ```

  The fields are uptime ms (8 wide), level (5 wide), tag (≤ 4 chars), and message.
- Tags: `SYS`, `CFG`, `NET`, `USB`, `PUMP`, `DISP`, `BTN`, `LED`, `WEB`, `OTA`, `SIM`.

### 12.2 What must be logged

- Boot banner, reset reason, config load source.
- Wi-Fi attempts, results, IP.
- USB enumeration, attach/detach.
- Every pump TX/RX line at VERBOSE. Transaction failures at WARN.
- Discovery results, level state changes.
- Every button press.
- Every web command received, with its client IP.
- Every dispense step (target, actual, deviation), abort, E-Stop.
- Config saves and validation failures, OTA progress/result.
- Every error raised or cleared.

### 12.3 Sinks

- **UART0** (D6, 115200), synchronous.
- **Ring buffer** of the last 300 lines in PSRAM. Sent to each new WebSocket client as `logHistory`.
- **WebSocket** broadcast of `log` messages, throttled to 50 messages/s. Overflow is collapsed into a `"... N lines dropped"` notice.

---

## 13. Web Server & HTTP Endpoints

The server listens on port 80 with no authentication (per v1). All responses are served from flash; there are no internet dependencies.

| Method & Path | Purpose |
|---|---|
| `GET /` | `index.html` (gzip) |
| `GET /app.js`, `/app.css`, `/favicon.svg` | Static assets (gzip, `Cache-Control: no-cache` + ETag = firmware build hash) |
| `GET /ws` | WebSocket upgrade (§14) |
| `GET /api/config` | Download `config.json` (`Content-Disposition: attachment; filename=drinkbot-config.json`) |
| `POST /api/config` | Upload/import a config (JSON body ≤ 32 KB). Validated, then saved. Returns `200 {ok:true}` or `400 {ok:false, errors:[...]}`. Rejected (409) during dispense. |
| `GET /api/info` | `{fwVersion, buildDate, uptimeMs, freeHeap, simulator, ip, wifiMode}` |
| `POST /api/ota` | Multipart firmware `.bin` upload. Rejected (409) unless READY/ERROR and idle. Progress goes out as `ota` WS messages. On success: log, reply, then reboot after 1 s. On failure: abort, stay on the current image. |
| *(any other)* | In AP mode with captive DNS: redirect to `/`. Otherwise 404. |

OTA validates the image header (ESP32-S3 chip ID) before committing. The bootloader's rollback is enabled: the new image marks itself valid only after the web server has started successfully.

---

## 14. WebSocket Protocol

- Endpoint `/ws`, text frames, one JSON object per frame, ≤ 8 KB (the `config` message may be up to 32 KB).
- Every message has a `"type"`. Client requests may include `"reqId"` (string). The server then answers with `ack` or `error` carrying the same `reqId`.
- The client reconnects with backoff (1 s, 2 s, 4 s, then 5 s max). On (re)connect the server sends `hello`, `state`, `pumps`, `config` and `logHistory`.

### 14.1 Server → client

| `type` | Payload | When |
|---|---|---|
| `hello` | `{fwVersion, buildDate, simulator, hostname}` | On connect |
| `state` | `{system: "READY"\|..., errors:[{code, pumpId?, message, latched}], activeRecipeId, activeVolumeMl, wifi:{mode, ssid, ip, rssi}, leds:[{r,g,b,mode:"solid"\|"blink"\|"fast"\|"off"} ×9]}` | On change (≤ 10/s) |
| `pumps` | `{pumps:[{pumpId, liquid, present, fwVersion, isBridge, weightG, levelMl, levelState:"OK"\|"LOW"\|"EMPTY"\|"UNKNOWN", activity:"IDLE"\|"DISPENSING"\|"PRIMING"\|"PURGING", led:{r,g,b,mode:1-4, brightness}}]}` | On change, plus every 3 s |
| `config` | full config object (§7.2) | On connect and after any successful save |
| `log` | `{t, level, tag, msg}` | Streaming |
| `logHistory` | `{lines:[{t, level, tag, msg}...]}` | On connect |
| `dispenseProgress` | `{recipeId, volumeMl, step, steps, liquid, pumpId, targetMl, actualMl?, status:"STARTING"\|"DONE"\|"COMPLETE"\|"ABORTED", reason?}` | During a dispense |
| `bulkProgress` | `{op:"primeAll"\|"purgeAll"\|"prime"\|"purge", step, steps, pumpId?, liquid?, durationMs?, estimatedTotalMs?, status:"STARTING"\|"RUNNING"\|"DONE"\|"SKIPPED"\|"COMPLETE"\|"ABORTED", reason?}` | During Prime All / Purge All, and a manual prime/purge (one step; a stopped run ends `DONE` with reason `stopped`) |
| `ota` | `{percent, status:"WRITING"\|"DONE"\|"FAILED", message?}` | During OTA |
| `ack` | `{reqId, data?}` | After a successful request |
| `error` | `{reqId?, code, message, details?}` | Request failed / was rejected |

### 14.2 Client → server

| `type` | Fields | Notes |
|---|---|---|
| `getConfig` | — | Replies with `config` |
| `saveConfig` | `{config}` | Whole-config replace. Validated per §7.3. The error `details` hold the failing field paths. |
| `setRecipe` | `{recipeId}` | |
| `setVolume` | `{volumeMl}` | |
| `dispense` | `{recipeId?, volumeMl?}` | Defaults to the active values |
| `prime` / `purge` | `{pumpId}` | Runs until `pumpStop` (at most 120 s). Stops any other manual run first (§9.6.1). |
| `pumpStop` | `{pumpId}` | Stops the manual prime/purge |
| `primeAll` | — | Sequential prime for each pump's `autoPrimeSec` (§9.6.2) |
| `purgeAll` | — | Sequential purge for each pump's `1.5 × autoPrimeSec` (§9.6.2) |
| `estop` | — | Highest priority |
| `clearErrors` | — | Clears latched errors |
| `rescan` | — | Rediscovery (§9.3) |
| `reboot` | `{force?: bool}` | |
| `setLogLevel` | `{level}` | Web sink only |
| `pumpTare` | `{pumpId}` | Maintenance |
| `pumpCalibrate` | `{pumpId, grams}` | Maintenance |
| `pumpSetId` | `{pumpId, newId}` | Sends `sid` then `scfg`. Updates config `pumpId` if the pump is in the list, then rescans. |
| `pumpLedTest` | `{pumpId}` | |
| `pumpRaw` | `{pumpId, command}` | Maintenance console. Rejected unless idle. The result lines come back in `ack.data.lines`. |

Error `code`s: `BUSY`, `INVALID`, `NOT_FOUND`, `PRECHECK_FAILED`, `PUMP_TIMEOUT`, `PUMP_ERROR`, `CONFIG_INVALID`, `INTERNAL`.

---

## 15. Web Application UI

### 15.1 General

- The app is a single page with no external resources, and it works offline.
- It is responsive, down to 360 px wide (phones). On phones, tabs become a dropdown or bottom bar.
- The connection indicator shows **Connected / Reconnecting**. While disconnected, controls are disabled.
- Destructive actions (Reboot, Import config, OTA, Set pump ID, Purge) ask for confirmation.
- Unsaved edits in an editor tab show a "● unsaved" marker. Leaving the tab or page warns.

### 15.2 Layout

```
+--------------------------------------------------------------------------+
| DrinkBot  [SIMULATOR]   Wi-Fi: STA HomeWiFi 192.168.1.40   ● READY        |
|                                          [ Reboot ]   [  E-STOP  ] (red) |
+--------------------------------------------------------------------------+
| Dashboard | Recipes | Pumps | Buttons | Wi-Fi | Settings | Maintenance   |
|                                                                          |
|   (active tab content, scrolls independently)                            |
|                                                                          |
+=================== drag handle (resize) =================================+
| Log  [level ▾] [Auto-scroll ✓] [Clear]                                   |
| [  123456] INFO  DISP - Step 2/3 ...                                     |
| ...                                                                      |
+--------------------------------------------------------------------------+
```

- **E-STOP** is always visible in the header, on every tab.
- **Log panel** is always visible at the bottom.
  - The height is set by dragging the handle with mouse or touch, from 80 px to 70 % of the window. It's saved in `localStorage`.
  - It auto-scrolls to the newest line. Auto-scroll pauses when the user scrolls up and resumes when they scroll back to the bottom, or tick the checkbox.
  - **Clear** empties the view only; the server ring buffer is unaffected. The client keeps at most 2000 lines.
  - Level colors: ERROR red, WARN amber, others default.

### 15.3 Dashboard

- **Dispense panel:** recipe selector (unavailable recipes are greyed out, with the reason as a tooltip), a total volume field in ml (default `defaults.volumeMl`, min/max from config), and a **Dispense** button.
  - During a dispense: a progress list with each step (liquid, target ml, actual ml, ✓/⚠/✗) and an overall progress bar.
- **Controller panel:** the 9 LEDs drawn as circles labeled with their button action (e.g. "Margarita", "250 ml", "Dispense", "E-Stop", "Status", "Wi-Fi"). Blink animations are done in CSS.
- **Pump cards** (one per configured pump):
  - Pump ID (plus a "bridge" badge), liquid name, firmware version, status (Present/Missing/FW too old/Busy), weight in g, and level in ml with a bar that has threshold markers.
  - A level badge (OK/LOW/EMPTY), and an LED-strip preview (a row of 20 dots animated for modes 1–4 in the tracked color).
  - **Prime** and **Purge** buttons. While that pump is priming, the Prime button becomes **■ Stop prime** (likewise for purge) and an elapsed counter shows "n / 120 s". The other cards' buttons stay enabled; pressing one stops the running pump first (§9.6.1).
  - The auto prime time, shown as "Auto prime: 4.0 s / purge 6.0 s", or "Auto prime: off" when it is 0.
- **Prime All** and **Purge All** buttons in a bar above the pump cards:
  - Both are disabled unless the state is READY or degraded ERROR and at least one pump qualifies (§9.6.2).
  - Clicking opens a confirm dialog listing each pump (ID, liquid, seconds), any pumps that will be skipped, and the estimated total time. Prime All reminds the user to put a container under the outlets. Purge All warns that it pushes liquid in the lines back to the bottles.
  - While running: a progress list (pump, liquid, seconds, ✓ / skipped / ✗) and an overall progress bar. The running pump's card is highlighted. E-STOP in the header aborts it.

### 15.4 Recipes

- A list of recipes with add, duplicate and delete. Each recipe has an editor for name, id (generated from the name, editable until first save) and an ingredient table.
- Ingredient table: liquid dropdown from the pump list (free text allowed), parts, order. Rows can be reordered by drag, which renumbers `order`.
- Live preview: "At 250 ml: Tequila 125.0 ml, Triple Sec 62.5 ml, …".
- **Save** sends the whole config through `saveConfig` and shows validation errors inline.

### 15.5 Pumps (liquids)

An editable table with columns pump ID, liquid, ms/ml, density g/ml, color picker, **auto prime (s)** (0–60, step 0.1, with a read-only "purge = 1.5×" value next to it), and live status/level. Rows can be added or removed. Saving triggers a rescan.

### 15.6 Buttons, Wi-Fi, Settings

- **Buttons:** 7 rows. Each has an action dropdown plus a recipe dropdown or volume field depending on the action.
- **Wi-Fi:** an ordered list of SSID and password (masked, with a show toggle). Rows can be added, removed and reordered. A note says changes apply after reboot.
- **Settings:**
  - Level thresholds, dispense tolerances and settle time, default volume/recipe/prime/purge, LED brightness, hostname, AP SSID.
  - **Export config** (download) and **Import config** (upload, then validate, then confirm).
  - **Firmware update**: choose a `.bin`, then a progress bar.
  - Device info.

### 15.7 Maintenance

- Pump selector.
- Tare, with the instruction "place the empty container on the scale".
- Calibrate: known weight in grams, with the instruction "place the known weight on the scale".
- Set ID.
- LED test.
- Raw command console: input and output pane.
- **Rescan pumps**, **Clear errors**.

---

## 16. Simulator Mode

- Entered when no USB CDC device is present at boot (§5.1). It is fixed for that session; a device plugged in later is logged, and a reboot is needed to use it.
- Shown by a "SIMULATOR" banner in the web header, `hello.simulator=true`, and a boot log line.
- `SimPumpLink` implements `IPumpLink` and models **8 virtual pumps**.
  - IDs are the configured `pumpId`s. If fewer than 8 are configured, more are added with IDs 1–8, skipping IDs already in use.
  - The first configured pump is the "bridge".
- Each virtual pump models:
  - firmware version `0.4.0-sim`;
  - a starting weight of 750 g × density;
  - `dispense` / `prime` lowering the weight over time at `1/msPerMl` ml/ms (purge raises it);
  - `status`, the LED state, and `tare`/`cal`/`sid` stubs;
  - realistic latency: 5 ms direct, 25 ms bridged.
- **Fault injection** (Maintenance tab, simulator only): mark a pump missing, inject a timeout on the next command, or set a pump's weight.
- All controller logic above the `IPumpLink` interface is the same in simulator and real modes.

---

## 17. Error Catalog

| Code | Severity | Raised when | Cleared when |
|---|---|---|---|
| `E_USB_LOST` | Blocking | CDC device detached at runtime | Re-enumeration plus successful discovery |
| `E_BRIDGE_UNRESPONSIVE` | Blocking | Bridge pump doesn't answer `version` | Rescan succeeds |
| `E_PUMP_MISSING(id)` | Degraded | No answer at discovery, or 3 poll failures | That pump answers |
| `E_PUMP_FW(id)` | Degraded | Pump version < 0.4.0 | Rescan finds ≥ 0.4.0 |
| `E_CONFIG` | Degraded | `config.json` invalid; using backup/default | Successful config save |
| `E_DISPENSE_TIMEOUT(id)` | Latched | Pump still running after expected time + 5 s | Clear Errors |
| `E_PUMP_COMM(id)` | Latched | Transaction failure during a dispense or manual op | Clear Errors |
| `E_STOP_UNCONFIRMED(id)` | Latched | Pump didn't acknowledge `stop` | Clear Errors |

**Blocking:** no dispense, prime or purge. **Degraded:** operations that don't involve the affected pump are allowed. **Latched:** dispensing is allowed again only after Clear Errors: from the web UI, or by holding the E-Stop button for 3 s (D-4).

Pre-check failures (insufficient liquid, unknown recipe, liquid not loaded) are **not** system errors. They are reported to the requester and logged at WARN.

---

## 18. Timing & Limits Constants

| Constant | Value |
|---|---|
| `USB_DETECT_TIMEOUT_MS` | 3000 |
| `DIRECT_TIMEOUT_MS` | 500 |
| `BRIDGE_TIMEOUT_MS` | 2500 (pump: 2000) |
| `WIFI_CONNECT_TOTAL_MS` | 20000 |
| `POLL_INTERVAL_MS` (per-pump weight cycle) | 3000 |
| `STATUS_POLL_MS` (during pour) | 250 |
| `DISPENSE_OVERRUN_MS` | 5000 |
| `settleMs` (config default) | 1500 |
| Button debounce | 30 ms |
| Button press flash | 250 ms |
| `LONG_PRESS_MS` (E-Stop hold to clear errors) | 3000 |
| LED refresh | 20 ms |
| Blink / fast blink | 500/500 ms, 150/150 ms |
| Prime/purge range (single pump) | 1–60 s |
| `autoPrimeSec` range | 0–60 s, 0.1 s steps (0 = skip). Purge All runs up to 90 s per pump. |
| `MANUAL_RUN_MS` | 120000 ms: a manual prime/purge runs this long unless stopped (= pump cap PF-9). |
| `BULK_GAP_MS` (pause between pumps in Prime/Purge All) | 500 |
| Volume range (config default) | 10–1000 ml |
| Max pumps / recipes / ingredients per recipe / networks | 16 / 64 / 16 / 8 |
| Log ring buffer / client lines | 300 / 2000 |
| Max WS clients | 4 |

---

## 19. Acceptance Tests

1. **Simulator boot:** with no USB device, the controller boots into simulator mode with 8 pumps, and the UI shows the banner.
2. **Volume math:** 250 ml of 2:1:1 pours 125 / 62.5 / 62.5 ml in `order`.
3. **Pre-check:** a recipe that would take any pump below `actualEmptyMl` pours nothing and returns `PRECHECK_FAILED` naming the liquid.
4. **E-Stop mid-pour:** all pumps report `IDLE` within 500 ms, the remaining steps are skipped, and the LEDs are restored.
5. **Missing pump:** with one configured pump absent, the status LED blinks red. Recipes without that pump dispense; recipes with it are blocked. Rescan after reconnecting clears the error.
6. **Button LEDs:** pressing the recipe button gives a 250 ms green flash, then red, and the other recipe LEDs go off. Setting the volume from the web lights the matching volume button.
7. **Wi-Fi fallback:** with no reachable networks, the open `DrinkBot` AP appears within about 21 s and the Wi-Fi LED is solid yellow. The UI loads at `192.168.4.1` and `drinkbot.local`.
8. **Config robustness:** a corrupted `config.json` boots from the backup with `E_CONFIG`, and invalid saves are rejected with field paths.
9. **Low/empty LEDs:** a pump crossing `lowMl` shows a slow yellow pulse, and crossing `emptyMl` shows a slow red pulse. The pulse returns after every dispense.
10. **OTA:** uploading a new `.bin` reboots into the new version, and a bad image is rejected while the old one stays.
11. **Log panel:** it stays visible on every tab, can be resized by drag, auto-scrolls, Clear works, and a new browser sees the last 300 lines.
12. **Clear by long press:** after an injected `E_DISPENSE_TIMEOUT`, holding E-Stop for 3 s clears it, the button LED blinks green 3×, and dispensing works again. A short press clears nothing.
13. **Idle pump LEDs:** a pump at OK level is dark when idle, and goes dark again after each dispense.
14. **Prime All:** with pumps set to `autoPrimeSec` 4, 0 and 2, the pumps run in ascending ID order for 4 s and 2 s, and the pump set to 0 doesn't run. A missing pump is listed as skipped and the rest still run.
15. **Purge All:** the same pumps run in reverse for 6 s and 3 s (1.5×).
16. **Prime/Purge All abort:** E-Stop during the second pump stops it within 500 ms, and later pumps don't run. A dispense requested during the sequence is rejected with `BUSY`.
17. **Schema migration:** a version 1 config loads with `autoPrimeSec` = `defaults.primeSec` on every pump, and is saved as version 2.
18. **Real hardware:** a direct `version` to the bridge pump and a bridged `version` to every pump succeed, and bridged CRCs verify.
19. **Manual prime/purge:** Prime on pump A runs until Stop prime is pressed, and ends by itself after 120 s. Pressing Purge on pump B while A is priming stops A before B starts; at no time do both run. Dispense and Prime All are rejected with `BUSY` while a manual run is active.
20. **Dispense by time:** a pour of `ml` on a pump with `msPerMl` sends `dispense <round(ml × msPerMl)> 3`; no `smilli` is sent. A pour needing more than 120 s is rejected by the precheck.

---

## 20. Glossary

| Term | Meaning |
|---|---|
| Bridge pump | The DrinkBotPump connected to the controller by USB. It dispenses its own liquid and relays commands to the RS485 bus. |
| Direct / bridged | A command sent straight to the bridge pump, or wrapped in `bridge <id> …` for a pump on the bus. |
| Parts | A unitless ratio of an ingredient within a recipe. |
| Level | Remaining liquid in ml: weight ÷ density. The scale is tared with the empty container. |
| Low / Indicated Empty / Actually Empty | The warning threshold / the red-warning threshold / the unpumpable reserve (A-3). |
| Degraded / Blocking / Latched | Error severities (§17). |
