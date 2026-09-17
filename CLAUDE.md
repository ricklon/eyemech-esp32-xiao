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

**Status: running on a XIAO ESP32-S3, calibrated, and moving.** Boot, I²C to
the PCA9685 at 0x40, the register readback, NVS save/load, the serial console,
the WiFi profile sweep, the AP fallback and mDNS are bench-verified. All six
axes have been driven under power and remeasured after the rebuild — LR 42/138,
UD 40/140, TL 90/13, BL 93/172, TR 90/172, BR 90/15, recorded in `af314e3` — and
the animations have been run and tuned on the mechanism (`a9899d7`). Joining a
network by name and the console keystroke echo are verified on hardware too. The
control page is the surface actually in use.

**Those measured limits live in NVS, not in the source.** The defaults table in
`eye_servo.c` is still Will Cogley's, so a board with erased NVS starts from his
linkage geometry rather than this mechanism's. Erasing NVS means recalibrating
before anything is driven, and the safe-boot flag exists for exactly that
window.

Still unproven: the PCA9685 oscillator has never been scoped, and the per-servo
pulse range is still the 500–2500 µs default. The C6 builds but has had no bench
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
runtime over the serial console (`!wifi set <ssid> <password>`), which joins the
network and writes it to NVS only if it works. The four profiles are a FIFO, so
no slot number is involved; `!wifi set <n> <ssid> <password>` still writes a
specific slot without testing it. `pio run -t menuconfig` → *eyemech networking*
can seed profile 1 for a first boot; that lands in the gitignored `sdkconfig`,
never in `sdkconfig.defaults`. Never paste credentials into a commit message or
a doc.

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

**`servo_limits` entries can run backwards**, and *which* entries changes.
In the compiled defaults it is `BL` and `TR` at `(90, 10)` — max numerically
smaller than min. On the rebuilt mechanism it is the other pair: the measured
limits in NVS have `TL` at `(90, 13)` and `BR` at `(90, 15)` running backwards
while `BL` and `TR` run forwards (`af314e3`: TL and BR open downward). Which
pair is inverted depends on how the horns went back on, so never assume it from
either table. Any code that clamps against these must handle either ordering;
`eye_motion.c` does it with `fminf`/`fmaxf`. Never "fix" a table by swapping
the values — that just moves the inversion somewhere less obvious.

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

**On USB-Serial-JTAG, `fflush()` is not enough to get a character out.** The
console runs on that peripheral with no VFS driver installed, and in that mode
IDF only raises the TX FIFO's flush bit on a `'\n'`. `fflush()` moves the byte
into the FIFO and leaves it there, so anything printed mid-line — a keystroke
echo, a progress dot — is invisible until a newline flushes the whole line at
once. `fsync(fileno(stdout))` is what actually flushes; `eye_console` wraps the
pair as `push_stdout()`.

**`esp_wifi_disconnect()` does not take effect before the next line of C.**
Setting a station config while an association is in flight is refused (*"sta is
connecting"*), and `esp_wifi_connect()` while a link is still up is ignored
(*"sta is connected, disconnect before connecting to new ap"*). The second one
is silent and dangerous: the old network stays connected, its next IP event
arrives, and untested credentials look like they worked. `sta_connect()` polls
`esp_wifi_sta_get_ap_info()` until the radio agrees it is down before
reconfiguring. Do not replace that with a fixed delay.

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
3. ~~**`servo_limits` per axis**~~ — **measured on this mechanism 2026-09-05**
   and recorded in `af314e3`. The values are in NVS; the defaults compiled into
   `eye_servo.c` are still Will Cogley's, so this only holds for a board whose
   NVS has not been erased. Seat every horn at closed-90 before remeasuring —
   measuring as-found horns is what broke a lid arm.
4. ~~**Which servos are actually fitted**~~ — **MG90S**, confirmed at the bench
   2026-09-05. Size the supply for 6 V / 4–5 A. Metal gears do not strip the way
   SG90s do; an MG90S driven into a stop keeps pushing until the horn, linkage
   or printed part fails instead, so binding is *more* costly here, not less.

The pot endpoints the MicroPython build needed are no longer relevant: the pots
are gone.

## Safety

A servo driven past a mechanical stop stalls, heats and strips its gears within
seconds, and these servos have **no feedback** — nothing reports position, load
or current, so there is no stall detection and the only sensor is you watching
the linkage. Bringing up a new axis: one servo at a time, unloaded first, small
steps, hand near the supply switch.

**After any mechanical change, establish direction with ONE 2° step before
jogging.** A lid sits against its closed stop by definition, so the closing
direction has zero headroom — unlike a gaze axis, which has travel either side of
centre. Jogging a lid 10° the wrong way is 10° into the eye or the frame, and
MG90S have the torque to break a printed part rather than stall. A lid arm was
broken exactly this way on 2026-09-05: horns were refitted, TL was jogged +10
three times on the assumption that + opened, and + was closing.

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
