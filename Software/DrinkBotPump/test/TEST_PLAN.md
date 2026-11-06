# DrinkBotPump Firmware 0.7.1 — Hardware Test Plan

This plan verifies the pump firmware on real boards before the controller (YD-ESP32-S3) exists. The PC stands in for the controller: it talks to each pump over USB and uses one pump as the RS485 bridge to the other.

Most checks are automatic (`pump_test.py` sends commands and checks replies, CRCs and timing). The script stops and asks you whenever a check needs eyes or hands: motor running, LED colors, weights, power cycling.

> **Power rule: only one pump may run at a time.** The pump supply is sized for one running pump. The script checks every other pump is `IDLE` before it sends any `dispense`, `prime` or `purge` (direct or bridged) and refuses otherwise. Do the same for any manual testing.

## 1. Bench setup

| Item | Setup |
|---|---|
| Pumps | Two pumps, **A** and **B**. A is the reference/bridge pump; B is the unit under test. |
| USB | Both pumps on USB to this Mac. |
| RS485 | A and B wired together: A↔A, B↔B, GND↔GND (plus termination if your boards need it). |
| Power | Each pump's motor supply connected as in normal use. |
| Liquid | Optional. Pump intake in a cup of water and outlet over another cup makes direction checks easy. Running dry is fine if your pump tolerates it; runs are short. |
| Scale | Load cell mounted, with the empty container on it. A known weight (e.g. 100–500 g object weighed on a kitchen scale) for calibration. |
| Firmware | 0.7.1, built with **Tools → Debug Port = Disabled** and **Flash Size = 2MB (Sketch: 1536KB, FS: 512KB)**. The script refuses to run on other versions. Every pump on a bus must run the same version (0.5.0 moved the bus to 9600 baud). |

**Flashing:** in the Arduino IDE open `DrinkBotPump.ino`, board *Raspberry Pi Pico*, Debug Port *Disabled*, Upload. (Claude can also flash from the command line with the IDE's bundled `arduino-cli`.)

**IDs:** the plan assigns IDs **1–8**, one per pump. Write the ID on each board with a marker once it's assigned.

## 2. How to run

```bash
python3 Software/DrinkBotPump/test/pump_test.py              # full suite, auto-detects the two pumps
python3 Software/DrinkBotPump/test/pump_test.py --suite quick  # shorter per-unit check (§4)
python3 Software/DrinkBotPump/test/pump_test.py --only T5,T10  # selected tests
```

Answer prompts with `y` (pass), `n` (fail, then describe what you saw) or `s` (skip). Each run writes a Markdown report to `Software/DrinkBotPump/test/reports/`.

For the scale alone, on a single pump with no bus: `python3 Software/DrinkBotPump/test/pump_test.py --scale-only` (tare, calibration, a second weight, 30 s stability, two untouched power cycles). Weights are measured against the empty reading just before them, because a slow zero shift of a few grams is normal for this cell.

## 3. Full suite (first pair)

| # | Test | Method | Pass criteria |
|---|---|---|---|
| T0 | Identify & versions | Auto + you | Both report `P,VER,0.7.1`. A lights green, B lights blue so you can label them. |
| T1 | Clean USB output | Auto | No unsolicited bytes on either USB port while idle (proves Debug Port = Disabled). |
| T2 | ID assignment | Auto | `sid 255`, `sid 300`, `sid abc` → `R,ERR`. `sid <n>` → `R,OK`, `gid` → `P,ID,<n>`, `scfg` → `R,OK`. A and B get distinct IDs. |
| T3 | Command parser | Auto | Unknown command → `R,INVLD`; blank line → no reply; `\r\n` endings work; missing argument → `R,INVLD`; a 400-char line doesn't hang the parser. |
| T4 | Motion limits | Auto | `dispense/prime/purge 0`, `… 120001`, `dispense abc` → `R,ERR`, pump stays `IDLE`. |
| T5 | Forward run & timing | Auto + you | `dispense 3000` → `R,OK`, `status` = `FWD`, ends at 3000 ms ±100 ms. You confirm the motor ran forward. |
| T6 | Reverse, busy, stop | Auto + you | `purge` → `REV` (you confirm reverse); a second motion while running → `R,ERR`; `stop` → `IDLE` immediately. |
| T7 | Max run accepted | Auto | `prime 120000` → `R,OK` with ~120000 ms remaining, then `stop`. |
| T8 | LED commands | You | Solid, fade, loop, bounce in the commanded color; `ledtime` turns the LEDs off after the current cycle; `ledmode 1 0` turns them off. |
| T9 | Dispense LED mode | Auto + you | `dispense 4000 3` chases during the run and the LEDs go dark when it ends. |
| T10 | Scale | Auto + you | `weight` is a number; `tare` → `R,OK`; `cal <W>` with the known weight → reads W ±1%; removed → ≈0 g; 10-reading noise < 1 g. Records how long `tare` takes. |
| T11 | Bridge A → B | Auto + you | `bridge B version/gid/status/weight` return `$[...]` packets with valid CRC32 ending in `$[R,OK|…]`; bridged `dispense` runs B (confirmed on B's own USB); bad bridged commands → `$[R,INVLD]` / `$[R,ERR]`. |
| T12 | Bridge errors | Auto | Absent ID → `R,TIMEOUT` after 3 s ±0.3 s; own ID, 0, 256 → `R,ERR`; second request while one is pending → `R,ERR`; >200-char command → `R,ERR`; nested `bridge` over the bus → `$[R,INVLD]`. B's USB stays silent throughout. |
| T13 | Bridged tare / cal | Auto | `bridge B tare` and `bridge B cal` return `$[R,OK]` before the 3 s bridge timeout (they take ≈1.8 s). |
| T14 | Broadcast (255) | Auto + you | `bridge 255 ledcolor/ledmode` changes both pumps, reply is only the local `R,OK`; `bridge 255 stop` stops B (over the bus) and, separately, A (locally) — one running at a time; `bridge 255 prime 1000` and `bridge 255 version` → `R,ERR` and B doesn't move. |
| T15 | Bridge B → A | Auto | T11's basic checks with the roles swapped (both RS485 directions work). |
| T16 | Bus soak | Auto | 300 × `bridge B status` with motors off, with B's motor running and with A's motor running, plus 300 × direct `status`: zero timeouts / CRC errors. Reports average and max latency (input for controller timeouts). |
| T17 | Persistence | You + auto | Power-cycle B (USB **and** motor supply). Boot animation: red fade for ~5 s then dark; GO LED toggles every 2 s. ID and scale calibration survive. |
| T18 | ID 0 (bridge board) | Auto | Bridged `sid 0` → `$[R,ERR]`. At ID 0 over USB: motion → `R,ERR`, it still bridges to the other pump, and its broadcast `stop` stops that pump. |

The final system's bridge is a dedicated board at ID 0. A and B here are both pumps, because T5, T6 and the other motor tests need them to be.

## 4. Quick suite (remaining six pumps)

Keep the first-pair **A** as the reference, swap each remaining pump into position **B**, run `--suite quick`, and give it the next ID (3, 4, … 8):

T0, T1, T2, T4, T5, T6, T9, T10, T11, T12 (timeout case only), T16 (100 round trips), T17.

## 5. Recording results

The report lists every check as PASS / FAIL / SKIP with the actual replies and timings. Anything that fails gets a note with what you observed. Keep one report per pump; the file name includes the IDs.

## 6. Results so far

| Date | Firmware | Finding |
|---|---|---|
| 2026-11-05 | 0.4.0 | 106/114 pass. Bus at 115200: 0 errors with motors off; 0.013% with one motor running; ~0.4% with two (bit flips, all caught by CRC). Bridged tare 1.8 s vs a 2 s bridge timeout. A packet right after power-up noise was lost. Fade glitch at phase boundaries. Scale signal far too small (wiring under investigation). → fixed in 0.5.0 except the scale. |
| 2026-11-05 | 0.5.0 | Bus at 9600 baud: 0 errors in 2,335 requests with one motor running, about 68 ms per bridged request. |
| 2026-11-05 | 0.6.0 | Bus at 50 kbaud: 0 errors in 16,922 requests with one motor running, about 14 ms per bridged request. Firmware update through the bus: 118 KB in 44 s with no retries (213 s at 9600). |
| 2026-11-05 | 0.6.0 | New pump with the electrical correction: scale at 377 counts/g, noise 0.12 g idle, 0.34 g motor on, up to 0.48 g LEDs on. Settles within about 2 s. Zero held through two untouched power cycles (under 0.2 g). |

| 2026-11-05 | 0.7.0 | A board at ID 0 as the bridge, with pump 1 on the bus: firmware update through it 118 KB in 44 s with no retries; 2,181 requests with no failures and motors idle; broadcast stop halted a running pump 71/71. Bus errors around motor start: 4 of 75 start replies corrupted (CRC caught all), and 1 of 25 `status` requests sent 50 ms after start got no reply; none at 200 ms. |

**Open scale items, deferred on 2026-11-05:**
- Readings run about 2% high after calibration (50 g added 51.06 g, 100 g added 101.78–102.3 g). Settling is ruled out. Next: compare placements on the container, then check the objects' true weights.
- One unexplained dip in a 60 s hold, consistent with a single bad sample about 730 g off. It didn't repeat in 1,195 samples. Possible safeguard: report the median of the last 3 samples.
- Readings shift up about 0.5 g with LEDs on.
- Pumps 1 and 2 (and any others without the correction) have no usable scale signal.

**PCB fault behind the scale problem:** the load cell's bias voltage output is connected to the HX711's feedback pin instead of its AVDD pin. The correction is to cut that trace and add a small wire to the AVDD pin. A corrected pump gives 377 counts/g, against about 0.06 before. An uncorrected pump reads near zero raw counts and barely responds to weight. The PCB design needs this fix before any new boards are made.

## 7. Firmware update tests

| # | Test | Method | Pass criteria |
|---|---|---|---|
| F1 | Bad input | Auto | Out-of-order and malformed chunks, wrong MD5, junk, compressed, other-layout and untagged images are all refused and nothing is staged. Motor commands are refused during an update. |
| F2 | Full update | `pump_update.py` | Over USB and through the bus: the version changes, the ID stays, no pump needs USB recovery. |
| F3 | Power cuts | `fw_interrupt_test.py` (guided) | A cut during a transfer leaves the old firmware running. A cancelled image doesn't install. A staged image installs on the next boot. A cut during the install completes on the next power-up. |

## 8. Out of scope here

- CRC-mismatch rejection on the bus (needs a raw RS485 adapter to inject bad packets; covered by code review).
- Multi-drop with more than two pumps on one bus — run once all eight have IDs, after this plan, by chaining them and bridging to each ID from A.
- Flow-rate calibration (ms/ml): that's the controller's job, done in M8.
