# DrinkBot Pump Controller

The `DrinkBotPump` firmware operates a networked liquid dispensing node designed for a custom control board based on the Raspberry Pi RP2040. It manages a reversible pump for precise liquid delivery, reads real-time weight feedback via an HX711 load cell, and drives a 20-LED NeoPixel strip for visual status indicators. The system operates either as a standalone device connected to a PC via USB (UART), or as part of a multi-node RS485 shared bus network. The same firmware also runs on the dedicated bridge board.

## Hardware & Build Requirements

*   **Target Board:** Raspberry Pi Pico (RP2040)
*   **Board settings:** Flash Size **2MB (Sketch: 1536KB, FS: 512KB)**, Debug Port **Disabled**. The file system holds incoming firmware updates. FQBN: `rp2040:rp2040:rpipico:dbgport=Disabled,flash=2097152_524288`
*   **IDE:** Arduino IDE version 2.3.10 or greater
*   **Required Libraries:**
    *   **HX711** by Rob Tillaart (0.6.x)
    *   **Adafruit NeoPixel** by Adafruit
    *   **ArduinoLog** by Thijs Elenbaas

## Configuration

Core behaviors are defined at the top of `DrinkBotPump.ino` using `constexpr` constants for easy modification:
*   `kLedBootAnimationTimeMs` (Default 5000ms) - Duration of the red boot-up visual.
*   `kBusTag` (Default `DBP-BUS:50000`) - Serial baud rate for RS485 data transmission. Every pump on a bus must use the same rate.
*   `kBridgeTimeoutMs` (Default 3000ms) - Time the pump waits for the final response when acting as a UART-to-RS485 bridge. It must exceed the ~1.8 s that a bridged `tare` or `cal` takes.
*   `kScaleTaskIntervalMs` / `kScaleReadingsPerMeasurement` - Minimum interval between background weight readings, and readings averaged per pass. The loop never waits for the HX711: it reads only once a sample is ready (10 samples/s).
*   `kScaleActionWaitTimeMs` / `kScaleCalibrationReadings` - Higher-precision read configurations used strictly when zeroing (tare) or calibrating the scale.
*   `kMaxMotionMs` (Default 120000ms) - Safety cap on any single pump run. Any `dispense`, `prime` or `purge` that would run longer is rejected.
*   `kBroadcastAddress` (Default 255) - RS485 address that every pump obeys without replying.

## Power: One Pump at a Time

The pump power supply is sized for **one running pump**. The system must never run two pumps at the same time. The firmware doesn't enforce this across the bus (a pump can't see the others' state); the controller, and any test tool, must make sure the previous pump is stopped before starting another.

## Firmware Update

Firmware can be updated over USB or through the RS485 bus, using the core's `Updater` and the OTA loader linked into every image:

```bash
# Build
arduino-cli compile -b rp2040:rp2040:rpipico:dbgport=Disabled,flash=2097152_524288 --output-dir build .
# Update pump 2 through the pump on USB
python3 tools/pump_update.py build/DrinkBotPump.ino.bin --port /dev/cu.usbmodem14101 --id 2
# Update the pump on USB itself
python3 tools/pump_update.py build/DrinkBotPump.ino.bin --port /dev/cu.usbmodem14101
```

*   **From the controller:** upload the `.bin` under Maintenance → Pump firmware, then press **Update pumps**. This is what to use in the finished machine; `tools/pump_update.py` is for the bench.
*   **Timing:** about 45 seconds per pump over the bus at 50 kbaud, a few seconds over USB.
*   **Procedure:** update one pump at a time with no pump running, and update the bridge last.
*   **What the pump checks:** the whole image's MD5; that its first 12 KB (boot2 and the OTA loader) match the pump's own flash; and that it contains a `DBP-IMAGE:<version>` tag. So only uncompressed images built with the same Arduino core and flash layout are accepted. Anything else must be flashed over USB. Updates never rewrite the loader.
*   **If it's interrupted:** until `fwapply` the current firmware keeps running, and a partial image is simply discarded. A staged image that hasn't been aborted is installed on the next reboot, even without `fwapply`. If power fails during the install, the loader starts it again on the next power-up.
*   **Recovery:** the RP2040's ROM bootloader can't be overwritten. Hold BOOTSEL while plugging in USB and flash over USB. New firmware that boots but can't talk on the bus needs this too: there's no automatic rollback.
*   **Compatibility:** every version must keep the bus baud rate and these commands working, or the pumps can't be updated over the bus afterwards.
*   **Not secured:** any device on the bus can update a pump. The bus is internal to the machine.

## Bridge Board

The final system uses a dedicated pump board as the RS485 bridge, with **ID 0** to identify it as a bridge and not a pump. It runs this same firmware:

*   It relays with `bridge <id> <command>`, and accepts `stop` and the LED commands from broadcasts.
*   `dispense`, `prime` and `purge` return `R,ERR` at ID 0, and the bus can't address it.
*   ID 0 is also the factory default, so a pump with no ID yet behaves the same way until it's given one.
*   To set a board up as the bridge, send `sid 0` then `scfg` over USB, or leave a new board as it is.

On the bench, a pump with a non-zero ID can still act as the bridge.

## Debugging Setup

Debug features (such as a blocking wait for the USB Serial connection on boot, extra bridge packet logging, and diagnostic output routing) are controlled natively via the Arduino IDE menus rather than manual macros.

*   **To Enable Debugging:** Go to **Tools -> Debug Port** and select **Serial** (or your preferred hardware port). This defines the `DEBUG_RP2040_PORT` macro, enabling verbose logging routed to the selected port and forcing the board to wait for a serial monitor connection before booting.
*   **To Disable Debugging (Production):** Go to **Tools -> Debug Port** and select **Disabled**. This disables the boot delay and completely silences the logger to reduce overhead.

> **Production builds must use Debug Port = Disabled.** When the pump is connected to a DrinkBotController, its USB serial port carries the command protocol. Debug log lines on that port mix in with responses.

## Communication Protocols

The device actively listens on two separate serial interfaces and dynamically routes responses to the interface that originated the request.

**1. Standard UART (USB)**
A traditional text-based interface used for direct computer-to-device communication. Commands are space-delimited text strings terminated by a newline (`\n`) character.
*   **Example Input:** `dispense 5000\n`
*   **Example Response:** `R,OK\n`

**2. RS485 Shared Bus (CommBus)**
A robust, addressed, and error-checked protocol at 50 kbaud designed for a single master to communicate with multiple slave devices over a shared serial bus operating at the defined `kCommBusBaudRate`. It wraps standard UART commands in a specialized packet structure.
*   **Packet Structure:** `*[address|command|CRC32]`
*   **Response Structure:** `$[response|CRC32]`
*   **Behavior:** Devices only process packets matching their configured ID (address). Any CRC32 mismatch or unrecognized address is silently ignored.
*   **Resync:** `*` and `$` only ever start a packet. On either one the receiver discards any partial bytes it was holding, so line noise can't spoil the next good packet.
*   **Example:** `*[3|dispense 5000|D2A8F3B1]` (Command to device ID 3 to dispense for 5000 ms)

**3. UART-to-RS485 Bridging**
Any device connected directly to a computer (or a DrinkBotController) via USB can act as a network bridge using the `bridge` command. It sends a properly formatted RS485 packet out to the shared bus. Then it forwards every response packet from the targeted device back over UART, verbatim and one per line, until the device's final `R,<result>` packet arrives.
*   **No local acknowledgement:** the bridge does not send its own `R,OK` after transmitting. The forwarded packets are the response.
*   **Example:** `bridge 3 weight` returns
    ```
    $[P,WEIGHT,123.456000|<CRC32>]
    $[R,OK|<CRC32>]
    ```
*   **Timeout:** if the final packet does not arrive within `kBridgeTimeoutMs` (3 s), the bridge writes `R,TIMEOUT`.
*   **Rejected with `R,ERR`:**
    *   a target outside 1-255, or the bridge's own ID (address the bridge directly instead);
    *   a request while another bridged request is still pending;
    *   a bridged command over 200 characters.
*   The host should verify the CRC32 of each forwarded `$[...]` packet. The CRC covers `$[<payload>|`.

**4. Broadcast (address 255)**
A packet addressed to `255` is run by **every** pump on the bus, and no pump replies. Only `stop` and the LED commands (`ledcolor`, `ledmode`, `ledfade`, `ledchase`, `ledtime`) are accepted; any other command is ignored. `bridge 255 <command>` transmits the broadcast and also runs the command on the bridge pump itself. The bridge's local `R,OK` / `R,ERR` is the only response.

## Command Reference

All commands return `R,OK\n` on success, `R,ERR\n` on failure, or `R,INVLD\n` for unrecognized inputs. Data retrieval commands return specific `P,...` prefixes, followed by `R,OK`. Blank lines are ignored, and a trailing `\r` is tolerated.

**Configuration & Scale**
*   `sid <id>`: Set device ID (address). Must be 1-254 for a pump, or 0 for a bridge board (USB only, since a pump set to 0 over the bus could no longer be addressed); otherwise `R,ERR`. Use `scfg` to persist it.
*   `gid`: Get device ID (Returns: `P,ID,<id>`).
*   `tare`: Tare the scale and save the offset to EEPROM. Tare with the empty liquid container on the scale, so readings are liquid weight only.
*   `cal <weight>`: Calibrate the scale against a known weight and save to EEPROM.
*   `weight`: Read current scale weight (Returns: `P,WEIGHT,<float>`).
*   `scfg`: Explicitly save active config (ID and scale calibration) to EEPROM.

**Pump Operations**
Every motion command is rejected with `R,ERR` if the pump is already running, or if the run would last 0 ms or more than `kMaxMotionMs` (120 s).
*   **`dispense <ms> [ledMode]`**: Runs the pump forward for `<ms>` milliseconds (1 to 120000). The controller converts volume to run time using its own per-pump calibration; the pump has no ms/ml setting. If an optional `ledMode` (1-4) is given, that LED mode runs for the length of the pump cycle. The reply comes when the pump **starts**; use `status` to see when it finishes.
*   `prime <ms>`: Runs the pump forward for the specified milliseconds.
*   `purge <ms>`: Runs the pump in reverse for the specified milliseconds.
*   `stop`: Stops the pump immediately and cancels any dispense LED timer. Always returns `R,OK`.
*   `status`: Returns `P,STATUS,<IDLE|FWD|REV>,<remainingMs>`.

**LED Control**
*   `ledcolor <R> <G> <B>`: Set the base color (0-255 per channel).
*   `ledmode <mode> <brightness>`: Set mode (1=Solid, 2=Fade, 3=Loop, 4=Bounce) and max brightness (0-255).
*   `ledfade <in> <on> <out> <off>`: Set Mode 2 timings in milliseconds.
*   `ledchase <speed> <gradient>`: Set Mode 3 & 4 step speed (ms) and tail gradient length.
*   `ledtime <ms>`: Start a countdown timer for the specified milliseconds. When the timer expires, the currently active LED animation is allowed to finish its natural cycle before gracefully turning off (resets to Mode 1 at 0 brightness). `ledtime 0` cancels the timer, including the 5 s boot animation timer.

**Firmware Update** (see [Firmware Update](#firmware-update))
*   `fwbegin <size> <md5hex>`: Start receiving an image. Rejected with `R,ERR` if the pump is running or the image won't fit. Motion commands return `R,ERR` until the update ends.
*   `fwdata <offset> <base64>`: Next chunk (up to 192 bytes after decoding). Chunks must arrive in order; a repeat of the last chunk is acknowledged, not written twice. `R,ERR` if out of order, or if the first 12 KB don't match this pump's flash, which also ends the update.
*   `fwend`: Check the size, MD5 and image tag, then stage the image for install. Safe to repeat.
*   `fwapply`: Reply `R,OK`, then reboot and install the staged image.
*   `fwabort`: Cancel, including a staged image. Always returns `R,OK`.
*   `fwstat`: Returns `P,FW,<IDLE|RECEIVING|STAGED>,<received>,<size>`.

**System**
*   `bridge <id> <command>`: Bridge a standard command to a specific device (or `255` for broadcast) over RS485. See Communication Protocols above.
*   `version`: Returns firmware version (Returns: `P,VER,0.7.1`).

## Changelog

**0.7.1**
*   A motion command's reply goes out before the motor starts, because motor start-up current can corrupt bus traffic. `stop` still cuts the motor immediately.
*   The image carries its bus speed in a `DBP-BUS:<baud>` tag, which also sets the speed. The controller refuses an image with a different speed.

**0.7.0**
*   ID 0 identifies a dedicated bridge board: `sid 0` is accepted over USB, and motion commands are rejected at ID 0.
*   Bus-compatible with 0.6.0.

**0.6.0**
*   Firmware update over USB or the RS485 bus: `fwbegin`, `fwdata`, `fwend`, `fwapply`, `fwabort`, `fwstat`, and `tools/pump_update.py`.
*   RS485 bus runs at **50 kbaud** (was 9600). All pumps on a bus must run 0.6.0.
*   Needs the new flash layout with a file system: every pump must be flashed once over USB. The ID and scale calibration are kept.
*   Longer limits: input lines 255 characters, bridged commands 200 characters (was 63).

**0.5.0**
*   RS485 bus runs at **9600 baud** (was 115200) for better noise immunity. All pumps on a bus must run 0.5.0.
*   Bridge timeout raised from 2 s to 3 s, so a bridged `tare` / `cal` (~1.8 s) reliably completes.
*   The bus receiver resyncs on `*` / `$`, so a packet after line noise (or a pump powering up) isn't lost.
*   Smoother fade (LED mode 2): no more one-frame black flash at the peak or bright flash at the end of fade-out, gamma-corrected brightness, and the loop no longer stalls up to 100 ms waiting for the scale, which caused skipped LED frames.

**0.4.0**
*   `dispense` takes a run time in milliseconds instead of a volume. The controller does the volume-to-time conversion.
*   Removed `smilli` / `gmilli` and the stored ms/ml value. The EEPROM layout of the remaining fields is unchanged, so ID and scale calibration survive the update.
*   Not compatible with controllers that send `dispense <ml>`: such a controller would run the pump for that many milliseconds.

**0.3.0**
*   Added `stop` and `status` commands.
*   `dispense` accepts decimal volumes up to 1000 ml. Previously the volume was held in an 8-bit value, so `dispense 300` silently poured 44 ml.
*   `dispense`, `prime` and `purge` are rejected while the pump is running, and any run over 120 s is rejected.
*   Bridge forwards every response packet up to the final `R,` packet. It no longer sends a local `R,OK`, and it reports `R,TIMEOUT`.
*   Added broadcast address 255 for `stop` and LED commands.
*   `sid` accepts only 1-254.
*   Blank lines and `\r` no longer crash or confuse the command parser. The pump timer is safe across `millis()` rollover.

**0.2.0**
*   Initial networked release.
