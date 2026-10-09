# eyemech-esp32-xiao

Firmware for a 3D-printed [Will Cogley EyeMech
ε3.4](https://nmrobots.com/products/eyemech-%CE%B53-4-with-on-board-face-tracking),
driven by a Seeed XIAO ESP32 in place of the original electronics. Two gaze axes
and four eyelids on six servos, a browser control page, a serial console, and an
MCP server so an agent can drive it.

## The mechanism

The printed parts and linkages are Will Cogley's **ε3.4** design, the snap-fit
revision of his ε-series eye mechanism with the camera-ready eye, sold by NM
Robotics. This build is printed from his single-plate file, [`Eyemech
e3.4.3mf`](https://drive.google.com/file/d/1Lum91_CLT5T7coVKDd-vOT7qpXyEPTcT/view),
linked from the product page.

It runs **six MG90S** metal-gear servos. His ε3.4 kit ships TS90MD instead, which
are quieter.

The electronics are what this repo replaces. Instead of his Eye Mechanism
Controller Board β2.0 and the code in his repo, this build uses:

- a **Seeed XIAO ESP32-S3** (the C6 is supported too),
- a **PCA9685** 16-channel PWM board over I²C at `0x40`,
- a separate 6 V servo rail sized for 4–5 A,
- optionally a **Grove Vision AI** module over UART for face tracking.

No CAD or print files are in this repo. Earlier revisions (ε3.2) and his
controller code are in
[will-cogley/EyeMech_Epsilon](https://github.com/will-cogley/EyeMech_Epsilon),
which is licensed [CC BY-NC-SA
4.0](https://creativecommons.org/licenses/by-nc-sa/4.0/): attribution,
non-commercial, share-alike. The ε3.4 file carries no licence of its own; treat
it the same way. Commercial use goes through enquiries@willcogley.com.

## Status

Running on a XIAO ESP32-S3, calibrated, and moving. All six axes were measured
on this mechanism after a rebuild (LR 42/138, UD 40/140, TL 90/13, BL 93/172,
TR 90/172, BR 90/15), and the animations were tuned on it. Those limits live in
the board's NVS. The defaults compiled into the firmware are still Will Cogley's
reference values, so a board with erased flash needs recalibrating before
anything is driven.

Not yet done: the PCA9685 oscillator has never been scoped, the per-servo pulse
range is still the 500–2500 µs default, and the C6 builds but has had no bench
time. Grove Vision tracking is ported but unproven in C. See
[docs/roadmap.md](docs/roadmap.md).

Two implementations live here:

- **`micropython/`**: the original MicroPython version for the XIAO ESP32-C6.
  It is the reference for behaviour: where the C port differs, the Python is
  right unless [docs/decisions.md](docs/decisions.md) says otherwise.
- **everything else**: the ESP-IDF C port, where new work goes.

## Hardware

Pin map, power, the `/OE` arrangement and bring-up order are in
[docs/WIRING.md](docs/WIRING.md). Read it before wiring anything.

The short version: PCA9685 logic from the XIAO's 3V3 pin (which keeps I²C at
3.3 V with no level shifter), servos from their own 5–6 V rail sized for stall,
all grounds tied together, and 1000 µF across `V+` at the PCA9685.

`/OE` has a 10k pull-down to GND, so outputs are always enabled, and D10 is
**not wired** to it on this build. There is no hardware stop: the **servo rail
switch is the emergency stop**.

## Building the C port

```
pio run -e xiao_esp32s3 -t upload       # or -e xiao_esp32c6
pio device monitor                      # 115200
```

No credentials to fill in first. On a board with nothing stored, the firmware
brings up a recovery access point, `eyemech-setup` (password `eyemech123`).
Join it and browse to <http://192.168.9.1/>, or set a network over the serial
console:

```
!wifi scan
!wifi set "My Network" hunter2
```

The board tries the credentials straight away and stores them only if they reach
an IP. Four profiles are kept, oldest evicted first. Credentials go to NVS, so
changing networks never needs a reflash.

The access point stays up in every state, and mDNS advertises `eyemech.local`
on both interfaces. Details in [docs/OPERATION.md](docs/OPERATION.md).

## Running the MicroPython original

```
mpremote connect COM5 cp micropython/pca9685.py :
mpremote connect COM5 cp micropython/servo.py :
mpremote connect COM5 cp micropython/main.py :
mpremote connect COM5 repl
```

`mpremote devs` lists candidate ports. Ctrl-C stops `main.py`. The servos hold
their last position; `pca.all_off()` releases them.

## Modes

| Mode | Behaviour |
|---|---|
| `tracking` | Follows faces from the Grove Vision module; blinks on a timer |
| `auto` | Random gaze and blinks. The default with no vision module |
| `manual` | You drive the gaze from the page, `/api/look` or `!look` |
| `calibration` | Everything to 90° on entry, then direct per-servo writes only |
| `standby` | Nothing driven, no blinks, MCP motion refused. Where safe boot lands |
| `anim` | Plays a named animation, then returns to the previous mode |
| `follow` | Eases toward live poses from a tracker; entered by sending a pose |

Animations: `look`, `roll`, `side_eye`, `wink`, `surprise`, `sleepy`,
`double_take`.

With **safe boot** on, the board comes up released and in `standby`, so nothing
moves until a person engages it. Keep it on during any mechanical work.

## Control

Three ways in, all doing the same things:

- **Control page** at `http://eyemech.local/`, the one in daily use.
- **Serial console** at 115200: `!help` lists everything. It needs no network,
  so it still works when WiFi doesn't.
- **HTTP API**, wrapped for the shell by `tools/eyectl.py`.

| Method | Path | Body |
|---|---|---|
| GET | `/api/state` | — |
| POST | `/api/mode` | `{"mode":"auto"}` |
| POST | `/api/look` | `{"lr":90,"ud":90}`, degrees |
| POST | `/api/blink` | — |
| POST | `/api/blink_hold` | `{"ms":150}` |
| POST | `/api/blink_gap` | `{"min_ms":10000,"max_ms":20000}` |
| POST | `/api/blink_style` | `{"style":"both"\|"alternate"}` |
| POST | `/api/lid_trim` | `{"value":0.5}`, 0..1 |
| POST | `/api/lid_coeff` | `{"upper":0.8,"lower":0.4}` |
| POST | `/api/anim` | `{"name":"wink","repeat":1}`; negative repeat loops |
| POST | `/api/anim/stop` | — |
| POST | `/api/pose` | `{"lr":0.5,"ud":0.5,"lid_l":1,"lid_r":1}`, normalised 0..1 |
| WS | `/ws/pose` | the same pose JSON, streamed |
| POST | `/api/follow/stop` | — |
| POST | `/api/servo` | `{"servo":"TL","angle":120}`, calibration mode only |
| POST | `/api/jog` | `{"servo":"TL","delta":2}`, calibration mode only |
| POST | `/api/mark` | `{"servo":"TL","end":"max"}`, calibration mode only |
| POST | `/api/limits` | `{"servo":"TL","min":90,"max":13}` |
| POST | `/api/cfg` | `{"servo":"TL","min_us":500,"max_us":2500,"trim_us":0}` |
| POST | `/api/save` | — commits limits and cfg to NVS |
| POST | `/api/defaults` | `{"confirm":"defaults"}`, reloads compiled defaults (not saved) |
| POST | `/api/safeboot` | `{"on":true}` |
| POST | `/api/release` | — all servos limp; **latches** until engage |
| POST | `/api/engage` | — clears the latch |

A pose takes either `lid_l`/`lid_r` or all four of `lid_tl`, `lid_bl`,
`lid_tr`, `lid_br`.

### MCP

The board serves MCP at `POST /mcp`: look, blink, play and stop animations, set
mode, read state, and release. It cannot engage or calibrate. Those need a person
at the page or the console. Register it through the stdio proxy, which keeps the
tools listed while the board is unplugged:

```
claude mcp add eyemech -- python3 /path/to/tools/eyemech_mcp.py
```

## Calibrating

Full procedure and measured values in [docs/CALIBRATION.md](docs/CALIBRATION.md).
These servos have no position feedback, so calibration is command-and-confirm:
the firmware commands an angle and a person watches the linkage. The rules that
matter most, learned at the cost of a broken lid arm:

1. **Seat the horn before measuring.** Servo at 90, lid just closed.
2. **Confirm direction with one 2° step after any mechanical change.** A lid
   sits against its stop, so the closing direction has no headroom.
3. **The servo rail switch is the emergency stop.** `!release` needs a live MCU
   and a working I²C bus. The switch does not.

Some limit pairs run backwards (max below min), and which ones depends on how the
horns went on. That is deliberate. Don't swap them.

Calibration survives reflashing. It does not survive erasing flash or a change to
`partitions.csv`.

## Documentation

| Doc | What it covers |
|---|---|
| [docs/OPERATION.md](docs/OPERATION.md) | Power-up order, stopping, modes, network, what survives a reflash |
| [docs/CALIBRATION.md](docs/CALIBRATION.md) | Measuring limits, the rules that stop it breaking parts, measured values |
| [docs/WIRING.md](docs/WIRING.md) | Pin map, power, `/OE`, bring-up order |
| [docs/PORTING.md](docs/PORTING.md) | Function-by-function map from the Python to the C |
| [docs/decisions.md](docs/decisions.md) | Why things are the way they are |
| [docs/roadmap.md](docs/roadmap.md) | What is done and what is next |
| [AGENTS.md](AGENTS.md) | Guidance for coding agents working in this repo |

## License

TBD for this repo's code. The mechanism design is Will Cogley's; see the note
under [The mechanism](#the-mechanism).
