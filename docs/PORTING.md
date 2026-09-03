# Porting map: MicroPython → ESP-IDF

Where each piece of the original went, and what changed on the way. Keep this
current — it is how the next person checks the port against the reference.

## Files

| MicroPython | C port |
|---|---|
| `micropython/pca9685.py` | `components/pca9685/` |
| `micropython/servo.py` | `components/eye_servo/` |
| `micropython/main.py` — motion primitives, mode machine | `components/eye_motion/` |
| `micropython/main.py` — `class Comms` | `components/eye_vision/` |
| `micropython/main.py` — pots, switches, blink button | **removed**, replaced by `components/eye_web/` |
| `micropython/main.py` — pin constants | `components/board/include/board_pins.h` |

## Functions

| Python | C | Notes |
|---|---|---|
| `PCA9685.set_freq` | `pca9685_set_freq` | Same prescale arithmetic, same sleep-to-write dance |
| `PCA9685.set_us` | `pca9685_set_us` | `us <= 0` still means full off |
| `PCA9685.all_off` | `pca9685_all_off` | |
| `trim_oscillator` | `pca9685_trim_oscillator` | |
| `Servo.write` | `eye_servo_write` | Redundant-write skip preserved |
| `Servo.read` | `eye_servo_read` | Returns `NAN` instead of `None` |
| `Servo.release` | `eye_servo_release` | |
| `servo_limits` dict | `eye_servo_limits` / `eye_servo_set_limits` | Now persisted in NVS |
| `calibrate()` | `eye_motion_calibrate` | |
| `neutral()` | `eye_motion_neutral` | |
| `blink()` | `eye_motion_blink_now` | |
| `open_lid()` | `eye_motion_open_lid` | |
| `control_ud_and_lids()` | `eye_motion_control_ud_and_lids` | Arithmetic carried over verbatim, 0.8 / 0.4 coefficients intact |
| `update_eyelid_limits()` | `eye_motion_set_lid_trim` | Same ranges; input is now 0..1 from the web UI instead of a raw ADC value |
| `scale_potentiometer()` | **gone** | No pots |
| `calibrate_pots()` | **gone** | No pots |
| `Comms.grove_read()` | `eye_vision_poll` | Same string-scraping, same 4096-byte runaway guard |
| main `while True:` | `eye_motion` task at 100 Hz | Blink state machine and mode dispatch preserved |

## Behavior changes, deliberate

1. **Pots and switches removed.** `controller` mode became `manual`, driven by
   `POST /api/look`. Mode selection moved from the mode switch to
   `POST /api/mode`. The blink button became `POST /api/blink`. D0–D3, D8 and D9
   are free as a result.
2. **Calibration persists in NVS.** The original reflashed to change
   `servo_limits`.
3. **Second board target.** The S3 builds alongside the C6; pin differences are
   isolated in `board_pins.h`.
4. **`initialisation` mode dropped.** It existed to bounce back into
   tracking/auto after the mode switch was released; with no switch, mode
   transitions are explicit and `eye_motion_set_mode()` handles the `neutral()`
   that `initialisation` used to cover.

## Behavior changes, accidental

Nothing known — but nothing has been run on hardware either. Anything found here
should be logged in this section and then fixed or promoted to "deliberate".

## Still string-scraped, not parsed

`eye_vision_poll()` finds `"resolution"`, then `"boxes":`, then the first two
integers, exactly as the Python did. Replacing this with a real JSON parse is a
reasonable improvement, but it is a behavior change — make it deliberately and
record it in `decisions.md`.
