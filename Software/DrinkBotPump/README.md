# DrinkBot Pump Controller

The `DrinkBotPump` firmware operates a networked liquid dispensing node designed for a custom control board based on the Raspberry Pi RP2040. It manages a reversible pump for precise liquid delivery, reads real-time weight feedback via an HX711 load cell, and drives a 20-LED NeoPixel strip for visual status indicators. The system operates either as a standalone device connected to a PC via USB (UART), or as part of a multi-node RS485 shared bus network.

## Hardware & Build Requirements

*   **Target Board:** Raspberry Pi Pico (RP2040)
*   **IDE:** Arduino IDE version 2.3.10 or greater
*   **Required Libraries:**
    *   **HX711** by Rob Tillaart (0.6.x)
    *   **Adafruit NeoPixel** by Adafruit
    *   **ArduinoLog** by Thijs Elenbaas

## Configuration

Core behaviors are defined at the top of `DrinkBotPump.ino` using `constexpr` constants for easy modification:
*   `kLedBootAnimationTimeMs` (Default 5000ms) - Duration of the red boot-up visual.
*   `kCommBusBaudRate` (Default 9600) - Serial baud rate for RS485 data transmission. Every pump on a bus must use the same rate.
*   `kBridgeTimeoutMs` (Default 3000ms) - Time the pump waits for the final response when acting as a UART-to-RS485 bridge. It must exceed the ~1.8 s that a bridged `tare` or `cal` takes.
*   `kScaleTaskIntervalMs` / `kScaleReadingsPerMeasurement` - Minimum interval between background weight readings, and readings averaged per pass. The loop never waits for the HX711: it reads only once a sample is ready (10 samples/s).
*   `kScaleActionWaitTimeMs` / `kScaleCalibrationReadings` - Higher-precision read configurations used strictly when zeroing (tare) or calibrating the scale.
*   `kMaxMotionMs` (Default 120000ms) - Safety cap on any single pump run. Any `dispense`, `prime` or `purge` that would run longer is rejected.
*   `kBroadcastAddress` (Default 255) - RS485 address that every pump obeys without replying.

## Power: One Pump at a Time

The pump power supply is sized for **one running pump**. The system must never run two pumps at the same time. The firmware doesn't enforce this across the bus (a pump can't see the others' state); the controller, and any test tool, must make sure the previous pump is stopped before starting another.

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
A robust, addressed, and error-checked protocol at 9600 baud designed for a single master to communicate with multiple slave devices over a shared serial bus operating at the defined `kCommBusBaudRate`. It wraps standard UART commands in a specialized packet structure.
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
    *   a bridged command over 63 characters.
*   The host should verify the CRC32 of each forwarded `$[...]` packet. The CRC covers `$[<payload>|`.

**4. Broadcast (address 255)**
A packet addressed to `255` is run by **every** pump on the bus, and no pump replies. Only `stop` and the LED commands (`ledcolor`, `ledmode`, `ledfade`, `ledchase`, `ledtime`) are accepted; any other command is ignored. `bridge 255 <command>` transmits the broadcast and also runs the command on the bridge pump itself. The bridge's local `R,OK` / `R,ERR` is the only response.

## Command Reference

All commands return `R,OK\n` on success, `R,ERR\n` on failure, or `R,INVLD\n` for unrecognized inputs. Data retrieval commands return specific `P,...` prefixes, followed by `R,OK`. Blank lines are ignored, and a trailing `\r` is tolerated.

**Configuration & Scale**
*   `sid <id>`: Set device ID (address). Must be 1-254 (0 is the factory default and 255 is broadcast); otherwise `R,ERR`. Use `scfg` to persist it.
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

**System**
*   `bridge <id> <command>`: Bridge a standard command to a specific device (or `255` for broadcast) over RS485. See Communication Protocols above.
*   `version`: Returns firmware version (Returns: `P,VER,0.5.0`).

## Changelog

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
