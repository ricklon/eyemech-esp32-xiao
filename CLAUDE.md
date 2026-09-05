# CLAUDE.md — eyemech-esp32-xiao

Guidance for Claude Code working in this repo. When a line here turns out to be
wrong, fix it rather than working around it.

## What this is

Will Cogley's animatronic eye mechanism, being **ported from MicroPython to
ESP-IDF C**. Six servos on a PCA9685 over I²C, optional face tracking from a
Grove Vision AI module over UART.

`micropython/` holds the working original. It is the **reference
implementation** — when the C behaves differently from those three files, the
Python is right and the C is wrong, unless a deliberate change is recorded in
`docs/decisions.md`. Do not edit `micropython/` to make the C look correct.

**Status: builds and runs on a XIAO ESP32-S3.** Boot, I²C to the PCA9685 at
0x40, the register readback, NVS save/load, the serial console, the WiFi profile
sweep, the AP fallback and mDNS are all bench-verified with the servo rail off.
**Nothing has been verified with servos powered** — no axis has moved, and every
calibration constant is still Will Cogley's. The C6 builds but has had no bench
time.

## The port's three deliberate changes

Everything else is meant to be behavior-identical to the Python.

1. **Two boards, one codebase.** The original ran on a XIAO ESP32-C6. The port
   also targets the XIAO ESP32-S3. Every pin difference lives in
   `components/board/include/board_pins.h` and nowhere else. Never write a raw
   GPIO number outside that file.
2. **The pots and switches are gone.** Three ADC pots, an enable switch, a mode
   switch and a blink button were the MicroPython control surface. `eye_web`
   replaces all of them over WiFi, and `eye_console` over the USB cable. D0–D3,
   D8 and D9 are now free on both boards.
3. **Calibration persists.** The MicroPython build reflashed to change
   `servo_limits`; the C port stores limits and per-servo pulse config in NVS
   under namespace `eyemech`, editable from the control page.

## Build and flash

```
pio run -e xiao_esp32c6                 # build for the C6
pio run -e xiao_esp32s3 -t upload       # build and flash the S3
pio device monitor                      # 115200
pio run -e xiao_esp32c6 -t menuconfig   # promote keepers to sdkconfig.defaults
```

WiFi credentials are no longer compiled in — `secrets.h` is gone. Set them at
runtime over the serial console (`!wifi set 1 <ssid> <password>`), where they go
to NVS. `pio run -t menuconfig` → *eyemech networking* can seed profile 1 for a
first boot; that lands in the gitignored `sdkconfig`, never in
`sdkconfig.defaults`. Never paste credentials into a commit message or a doc.

`sdkconfig*` is generated and gitignored; `sdkconfig.defaults` is the checked-in
source of truth.

## Layout

```
src/main.c                boot sequence only — the /OE ordering lives here
components/board/         board_pins.h: the only file with GPIO numbers in it
components/pca9685/       port of micropython/pca9685.py, plus /OE control
components/eye_servo/     port of micropython/servo.py + the servo_limits table
components/eye_motion/    port of main.py's primitives and mode machine
components/eye_vision/    port of main.py's Comms class (Grove Vision over UART)
components/eye_net/       WiFi: permanent recovery AP + 4 NVS station profiles
components/eye_web/       HTTP control page; replaces the pots
components/eye_console/   serial control; the surface that works without WiFi
micropython/              THE ORIGINAL — reference, not dead code
docs/WIRING.md            pin map, power, bring-up order, calibration procedure
docs/PORTING.md           function-by-function map from Python to C
tools/eyectl.py           drive the HTTP API from a shell
```

## Things that will bite you

**`servo_limits` entries can run backwards.** `BL` and `TR` are `(90, 10)` —
max is numerically smaller than min, because those two lid servos are mounted
mirrored relative to their partners. Any code that clamps against these must
handle either ordering; `eye_motion.c` does it with `fminf`/`fmaxf`. Never
"fix" the table by swapping the values.

**`/OE` has a 10k pull-DOWN to GND on this build, not a pull-up.** Outputs are
therefore enabled by default, through the whole boot window. On a cold start the
PCA9685's own power-on reset zeroes the `LEDn` registers and sets `SLEEP`, so
there are no pulses and the servos are limp anyway. On a warm reset the ESP32
reboots but the PCA9685 does not: it retains its registers and keeps emitting the
last pulses, so the servos *hold* rather than going limp.

`/OE` was never what prevented the slam. The slam comes from `eye_motion_neutral()`
writing 90 degrees into all six channels at once, and no `/OE` state stops that.
Recovering the last commanded position from the `LEDn_OFF` registers and ramping
to neutral is what stops it — those registers are the only position memory that
exists, because these servos have no feedback.

What `/OE` is good for is the emergency release: driving D10 high makes every
output go low instantly, with no I2C transaction, so it still works when the bus
is wedged or the firmware has crashed. Driving high is safe against the 10k
pull-down. Do not gate the boot sequence with it — disabling outputs before
seeding positions makes the servos sag and then snap back.

**`eye_servo_write()` skips redundant writes.** The motion loop rewrites
identical lid targets constantly and I²C is the bottleneck; the skip is worth
roughly an order of magnitude in loop rate. Don't remove it.

**Timing is wall-clock, not loop counts.** The original's
`random.randrange(20000)` blink interval was tied to the Pico's loop rate and
did not survive the port to a different core. Use `esp_timer_get_time()`.

**The coefficients in `control_ud_and_lids()` are the character of the face.**
0.8 for the upper lids, 0.4 for the lower ones. Changing them changes how the
mechanism reads as alive. If you change one, record it in `docs/decisions.md`.

## What still has to be measured on hardware

Do not invent these, and do not carry anything over from the Pico version.

1. **PCA9685 oscillator** — clone boards ship 24–27 MHz instead of 25. If the
   servos sit consistently off centre, scope a channel while commanding 50 Hz
   and pass the result through `pca9685_trim_oscillator()`.
2. **Per-servo pulse range** — `eye_servo` defaults to 500–2500 µs. Servos that
   only honour 1000–2000 µs under-travel silently, which shows up here as lids
   that never fully close.
3. **`servo_limits` per axis** — the checked-in values are Will Cogley's and
   assume his linkage geometry.
4. **Which servos are actually fitted** — SG90 vs MG90S changes both the pulse
   range and the supply sizing (see `docs/WIRING.md`).

The pot endpoints the MicroPython build needed are no longer relevant: the pots
are gone.

## Safety

A servo driven past a mechanical stop stalls, heats and strips its gears within
seconds, and these servos have **no feedback** — nothing reports position, load
or current, so there is no stall detection and the only sensor is you watching
the linkage. Bringing up a new axis: one servo at a time, unloaded first, small
steps, hand near the supply switch.

`!release` on the serial console is the fast way to make everything go limp, and
it is the one that does not need the network. `POST /api/release` does the same
over HTTP. Both latch — nothing moves again, blinks included, until `!engage`.

Calibration is command-and-confirm, never capture-and-record: the firmware
commands a position and a human confirms it. Do not carry the calibration flow
over from `~/Projects/lerobot` — those Feetech servos report position and these
do not.

## Working style

- Small commits, one concern each.
- If a change can't be verified without hardware, say so in the commit body.
- No new dependencies without asking. ESP-IDF's built-ins cover everything here.
- C, not C++. ESP-IDF style: `esp_err_t` returns, `ESP_RETURN_ON_ERROR` at
  boundaries, one `TAG` per file, public API in `include/`, everything else
  `static`.

## Open questions for Rick

- Is the Grove Vision AI module part of the build going forward, or is auto +
  web control the real operating mode?
- Does this connect to the xiaozhi voice agent eventually — eyes reacting to
  agent state rather than to a camera?
- Is the C6 or the S3 the board this actually ships on? Both build today, but
  only one is going to get bench time.
