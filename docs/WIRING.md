# Eye Mechanism — XIAO ESP32-C6 / ESP32-S3 + PCA9685

## Files

For the MicroPython C6 original, copy all three files to the board's
filesystem: `pca9685.py`, `servo.py`, `main.py`. For the ESP-IDF C port, select
the board with the PlatformIO environment: `xiao_esp32c6` or `xiao_esp32s3`.

## Network setup

WiFi is `eye_net`'s job. The board keeps up to four station profiles in NVS and
sweeps them at boot; the first that reaches an IP becomes the default. The
**recovery access point is always up**, in every station state:

| Recovery AP | Value |
|---|---|
| SSID | `eyemech-setup` |
| Password | `CONFIG_EYE_NET_AP_PASSWORD` — set it in menuconfig; the default is public |
| URL | `http://192.168.9.1/` |

It sits on 192.168.9.1, not 192.168.4.1, because the bench LAN is 192.168.4.0/22
and the two collided.

On a first boot, or on a new network, join over the USB serial console:

```
!wifi scan
!wifi set "Some Network" thepassword
```

`!wifi set` tries the credentials live and saves them only if the station gets an
IP. The board then answers as `eyemech.local` over mDNS. See
[OPERATION.md](OPERATION.md#network) for profiles, eviction and the sweep.
Setting WiFi from the control page is not in this build yet.

## XIAO physical wiring

The C6 and S3 builds use the same physical XIAO silk pins. If you wire by
`D0`..`D10`, the PCA9685 and Grove Vision connections do not move when swapping
between boards.

| XIAO silk | Connects to |
|---|---|
| D0 / A0 | Free |
| D1 / A1 | Free |
| D2 / A2 | Free |
| D3 | Free |
| D4 (SDA) | PCA9685 SDA |
| D5 (SCL) | PCA9685 SCL |
| D6 (TX) | Grove Vision **RX** |
| D7 (RX) | Grove Vision **TX** |
| D8 | Free |
| D9 | Free |
| D10 | PCA9685 `/OE` (optional; not wired on this build — see below) |
| 3V3 | PCA9685 VCC, Grove Vision VCC |
| GND | Common ground (mandatory — see below) |

The pots, switches and buttons from the MicroPython C6 build are gone in the C
port; the browser control surface replaced them. D0-D3, D8 and D9 are free on
both boards.

### MicroPython original on the C6

The reference build in `micropython/` still uses the pots and switches, on these
pins. This table is for running that build, not the C port.

| XIAO | GPIO | Connects to |
|---|---|---|
| D0 / A0 | 0 | UD pot wiper |
| D1 / A1 | 1 | Trim pot wiper |
| D2 / A2 | 2 | LR pot wiper |
| D3 | 21 | Enable switch → GND |
| D4 (SDA) | 22 | PCA9685 SDA |
| D5 (SCL) | 23 | PCA9685 SCL |
| D6 (TX) | 16 | Grove Vision **RX** |
| D7 (RX) | 17 | Grove Vision **TX** |
| D8 | 19 | Mode switch → GND |
| D9 | 20 | Blink button → GND |
| D10 | 18 | PCA9685 `/OE` (optional — emergency release only) |
| 3V3 | — | PCA9685 VCC, pot high sides, Grove Vision VCC |
| GND | — | Common ground (mandatory — see below) |

All switches and buttons use internal pull-ups and switch to ground. No
external resistors needed on those.

Only GPIO0/1/2 are ADC-capable on this board, so the three pots are
locked to D0/D1/D2. Everything else is placed around them.

## Board GPIO maps

These are the GPIO numbers behind the same physical XIAO pins above. Firmware
uses `components/board/include/board_pins.h`, so do not move wires when changing
the PlatformIO environment; use `xiao_esp32c6` or `xiao_esp32s3` instead.

| XIAO silk | ESP32-C6 GPIO | ESP32-S3 GPIO | Role |
|---|---:|---:|---|
| D0 / A0 | 0 | 1 | Free |
| D1 / A1 | 1 | 2 | Free |
| D2 / A2 | 2 | 3 | Free |
| D3 | 21 | 4 | Free |
| D4 (SDA) | 22 | 5 | PCA9685 SDA |
| D5 (SCL) | 23 | 6 | PCA9685 SCL |
| D6 (TX) | 16 | 43 | Grove Vision **RX** |
| D7 (RX) | 17 | 44 | Grove Vision **TX** |
| D8 | 19 | 7 | Free |
| D9 | 20 | 8 | Free |
| D10 | 18 | 9 | PCA9685 `/OE` (optional) |

The XIAO user LED also differs: C6 uses GPIO15, S3 uses GPIO21. It is active
low on both boards and is handled in firmware only.

## PCA9685 channels

| Ch | Servo | Function |
|---|---|---|
| 0 | LR | Eye left/right |
| 1 | UD | Eye up/down |
| 2 | TL | Top-left lid |
| 3 | BL | Bottom-left lid |
| 4 | TR | Top-right lid |
| 5 | BR | Bottom-right lid |

Channels 6–15 are free.

## The `/OE` pull-down

This build has a **10k resistor from `/OE` to GND**. Note that this is a
pull-DOWN, the opposite of what earlier revisions of this document (and
`micropython/main.py`) called for.

`/OE` is active low, so a pull-down means **outputs are enabled by default**,
including through the entire boot window. Most Adafruit-pattern PCA9685
breakouts already carry an onboard pull-down for exactly this reason, so the
pin works when left unconnected. Do **not** also fit a pull-up to 3V3: against
an onboard pull-down it forms a divider that puts `/OE` near 1.65 V, between the
chip's V_IL max (~0.99 V) and V_IH min (~2.31 V), which is an indeterminate
input and an intermittent fault.

What this means at reset:

- **Cold power-on.** The PCA9685's own power-on reset zeroes the `LEDn`
  registers and sets `SLEEP`. No pulses are generated regardless of `/OE`, so
  the servos are limp.
- **Warm reset.** The ESP32 reboots; the PCA9685 does not. It keeps its
  registers and keeps emitting the last commanded pulses, so the servos hold
  position rather than going limp.

Neither case slams. The slam comes from firmware writing 90° into all six
channels at once, and no `/OE` state prevents that — see the bring-up order
below.

Because the resistor is 10k rather than a hard tie, D10 can safely drive `/OE`
high (about 0.33 mA through the resistor). That is worth wiring: it gives an
instant, asynchronous release that needs no I²C transaction, so it still works
when the bus is wedged or the firmware has crashed. It is the only stop that
survives a dead MCU.

Note that with `MODE2` set to `OUTDRV=1, OUTNE=00` — what `pca9685.c` writes —
disabled outputs are driven **low**, not high-impedance. Low is the right choice
for servos: a constant low is simply no pulse, where a floating line could pick
up noise the servos read as one.

## Power

Two separate rails, one shared ground.

**Logic (3.3 V):** PCA9685 `VCC` from the XIAO's `3V3` pin. That pin
sources up to 700 mA; the PCA9685 draws roughly 10 mA. Running VCC at
3.3 V also puts the I2C bus at 3.3 V, which matches the XIAO directly —
**no level shifter required**. If you power VCC from 5 V instead, you do
need one, so don't.

**Servos (`V+` terminal block):** external 5–6 V supply, **not** from the
XIAO. Size it for stall, not idle. SG90-class micro servos stall around
700 mA each; MG90S metal-gear around 1.2 A. Six of them will not all
stall at once in normal operation, but a jammed lid plus a startup surge
gets there fast.

- SG90-class: 5 V, 3 A minimum
- MG90S-class: 6 V, 4–5 A

**Lid arms come in A and B variants.** The part that couples the lid linkage to
the servo horn is printed in two sizes to suit different horn dimensions, and the
design includes a printed measuring card for working out which one you need.
Measure the horn against the card and print the matching variant — a reprinted
arm is not interchangeable if the horn differs. Worth checking before printing a
spare, since the arm is the part that has failed here.

**Do not use threadlocker or CA glue on the printed linkage parts.** Loctite-type
anaerobic threadlockers contain methacrylate esters that attack many
thermoplastics, and cyanoacrylate crazes others; both cause stress cracking that
shows up at exactly the loaded joints you were trying to secure. A set of
linkage joints was lost this way on 2026-09-05. Use mechanical retention
instead — nylon-insert nuts, a captive nut, or a friction fit — and if a
fastener must be secured, secure it against a metal part rather than the print.

**Print lid arms at 0.20 mm layer height in a strength preset.** These are small
parts under repeated load from a metal-geared servo with no stall detection, so
layer adhesion matters more than surface finish. The arm that broke was the
weakest link in the whole mechanism.

**Bring-up rule learned the hard way:** after fitting or refitting a horn, move
the servo by a *single 2° step* and confirm which way the lid travels before
jogging further. Direction is not predictable from the horn's appearance, and the
closing direction has no spare travel — the lid is already against something. A
lid arm was broken on 2026-09-05 by three 10° steps taken in the closing
direction on that assumption.

**This build uses MG90S**, so size for the 6 V, 4–5 A figure. Note what metal
gears change about failure: an MG90S pushed into a hard stop does not strip
its gearset the way an SG90 does — it keeps pushing, and what gives instead is
the servo horn, the linkage, or the printed part it is bolted to. The gearbox
surviving is not the same as the mechanism surviving, so the "small steps, stop
at the first sign of binding" rule matters *more* here, not less.

Put a **1000 µF electrolytic across `V+` and `GND` at the PCA9685
itself**, not back at the supply. Servo current transients are fast and
the wiring inductance between supply and board will otherwise show up as
brownout resets on the XIAO.

**Ground:** XIAO GND, PCA9685 GND, and the servo supply negative must all
be tied together. This is the single most common failure — without it the
PWM signal has no reference and servos twitch, buzz, or ignore commands.

## First bring-up, in order

1. Power the logic only. Confirm the PCA9685 answers on I2C at 0x40: `!status`
   over the serial console reports it. For the MicroPython original:
   - C6: `I2C(0, sda=Pin(22), scl=Pin(23), freq=400_000).scan()`
   - S3: `I2C(0, sda=Pin(5), scl=Pin(6), freq=400_000).scan()`

   Both should return `[64]`.
2. Servo supply on. Nothing should move: on a cold start the PCA9685 generates no
   pulses until something writes to it, and safe boot keeps every channel released.
3. Open the control page at `http://eyemech.local/`, or join `eyemech-setup` and
   use `http://192.168.9.1/` if no network is configured yet.
4. One servo on channel 0, one step at a time. The full procedure, including the
   one-2°-step rule after any mechanical change, is in
   [CALIBRATION.md](CALIBRATION.md).
5. Fit horns and linkages with everything at 90°, then add the rest.

## Calibration you still have to do

**Pot endpoints — MicroPython only.** The C port has no pots; `eye_web`
replaced them. This applies solely to running `micropython/main.py`: sweep each
pot end to end from the REPL with `main.calibrate_pots()`, then edit `POT_MIN`,
`POT_MAX`, `TRIM_MIN`, `TRIM_MAX` at the top of `main.py`. The original trim
range of 7000–14500 was already saturating against its own clamp on the Pico, so
don't carry those numbers over.

**PCA9685 oscillator.** Clone boards routinely ship with an oscillator
anywhere from 24 to 27 MHz instead of 25. If servos sit consistently off
centre, put a scope or logic analyser on any channel, measure the real
frequency while the code commands 50 Hz, then:

```python
from pca9685 import trim_oscillator
trim_oscillator(52.4)   # returns the corrected osc_hz to pass in
```

**Servo pulse range.** `servo.py` defaults to 500–2500 µs across 0–180°.
Some servos only honour 1000–2000 µs and will silently under-travel,
which on this mechanism shows up as lids that never fully close. Adjust
`min_us` / `max_us` per servo, and use `trim_us` for individual mechanical
centring offsets rather than fudging `servo_limits`.

## Notes on what changed from the Pico version

- `picozero` was a dead import — `Button` was never used. Removed.
- The XIAO C6 user LED (GPIO15) is **active low**, opposite the Pico's GPIO25.
  The S3 user LED is GPIO21 and is also active low. Polarity is handled by
  `board_pins.h`.
- Blink timing was tied to loop-iteration count
  (`random.randrange(20000)`), which doesn't survive a different core at a
  different clock. Now driven by `ticks_ms()`.
- I2C runs at 400 kHz. At the 100 kHz default, six servo updates cost
  roughly 3 ms and cap the loop near 330 Hz.
- `Servo.write()` skips redundant writes. The main loop rewrites identical
  lid targets constantly, and I2C is now the bottleneck rather than free
  GPIO toggling.
- Added a runaway-buffer guard in `grove_read()`. The original could grow
  `cbuf` without limit if the module stopped sending the resolution key.
- `auto` mode command 0 now reopens the lids after blinking; the original
  left them shut until the next command happened to open them.
