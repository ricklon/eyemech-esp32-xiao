# eyemech-esp32-xiao

Will Cogley's animatronic eye mechanism on a Seeed XIAO — six servos on a
PCA9685, optional face tracking from a Grove Vision AI module, and a browser
control page.

Two implementations live here:

- **`micropython/`** — the working original. XIAO ESP32-C6, MicroPython, three
  files copied to the board. This runs today.
- **everything else** — an ESP-IDF C port targeting both the C6 and the S3.
  Scaffolded, not yet compiled. This is where new work goes.

> The port builds and runs on a XIAO ESP32-S3, verified with the servo rail
> switched off: I²C, calibration storage, the serial console, WiFi and mDNS all
> work. **No servo has been driven yet** — calibration constants are still the
> original's and assume Will Cogley's linkage geometry. See
> [CLAUDE.md](CLAUDE.md) for what still has to be measured.

## Hardware

Pin map, power requirements, the `/OE` pull-down and the bring-up order
are all in [docs/WIRING.md](docs/WIRING.md). Read it before wiring anything.

The short version: PCA9685 logic from the XIAO's 3V3 pin (which keeps I²C at
3.3 V and removes any need for a level shifter), servos from a separate 5–6 V
rail sized for stall, all grounds tied together, 1000 µF across `V+` at the
PCA9685 itself, and a 10k pull-down from `/OE` to GND.

## Running the MicroPython original

```
mpremote connect COM5 cp micropython/pca9685.py :
mpremote connect COM5 cp micropython/servo.py :
mpremote connect COM5 cp micropython/main.py :
mpremote connect COM5 repl
```

`mpremote devs` lists candidate ports. Ctrl-C in the REPL stops `main.py`; the
servos hold their last position rather than going limp — `pca.all_off()`
releases them.

## Building the C port

```
pio run -e xiao_esp32c6 -t upload
pio device monitor
```

No credentials to fill in first. On a board with nothing stored, the firmware
brings up a recovery access point — `eyemech-setup`, password `eyemech123` —
and waits. Join it and browse to <http://192.168.4.1/>, or set a network over
the serial console:

```
!wifi scan
!wifi set "My Network" hunter2
```

The board tries the credentials straight away and stores them only if they reach
an IP, so a typo costs an attempt rather than a saved profile. Four profiles are
kept, oldest evicted first, and nothing has to name a slot. Credentials go to
NVS, so changing networks never needs a reflash.

## Networking

The access point is up in **every** state, including while a station link is
working, so the mechanism cannot become unreachable because a network changed.
Four station profiles are stored and swept in turn — three retries each, then
on to the next — and the list is re-checked every couple of minutes while
parked on the access point, so a network that drops out is picked back up
unattended. Only a profile that actually obtained an IP becomes the boot
default, so a typo does not survive a power cycle.

mDNS advertises `eyemech.local` on both interfaces, so the board answers to
that name whether it joined your network or you joined its access point — which
is what `eyectl` defaults to. `--host 192.168.4.1` is the fallback if mDNS is
blocked, as it is on some corporate networks.

## Modes

| Mode | Entered | Behavior |
|---|---|---|
| `tracking` | default when a Grove Vision module answers at boot | Follows detected faces, blinks on a wall-clock timer |
| `auto` | default when no vision module | Random gaze and blink patterns |
| `manual` | control page, or any `/api/look` | You drive the gaze |
| `calibration` | control page | Everything to 90° for fitting horns and linkages |

Every mode change runs `neutral()` and clears any half-finished blink.

## Control API

| Method | Path | Body |
|---|---|---|
| GET | `/api/state` | — |
| POST | `/api/mode` | `{"mode":"tracking"\|"auto"\|"manual"\|"calibration"}` |
| POST | `/api/look` | `{"lr":90,"ud":90}` — degrees |
| POST | `/api/lid_trim` | `{"value":0.5}` — the old trim pot, 0..1 |
| POST | `/api/blink` | — |
| POST | `/api/servo` | `{"servo":"TL","angle":120}` — calibration mode only |
| POST | `/api/limits` | `{"servo":"TL","min":90,"max":170}` |
| POST | `/api/cfg` | `{"servo":"TL","min_us":500,"max_us":2500,"trim_us":0}` |
| POST | `/api/save` | — commits limits and cfg to NVS |
| POST | `/api/release` | — all servos limp; **latches**, nothing moves until engage |
| POST | `/api/engage` | — clear the release latch and go to neutral |

`tools/eyectl.py` wraps these for the command line.

## Serial control

Every endpoint above needs a network. The serial console does not, which matters
because a mechanism that can strip its own gears should not become
uncontrollable when WiFi fails to associate. Connect at 115200 and type `!help`:

```
!status                 mode, release latch, vision, trim, heap, servo table
!release / !engage      emergency stop (latched), and the way back
!mode calibration
!jog TL +5              step from the last commanded angle
!mark TL max            record where it is now as the open endpoint
!save                   commit to NVS
```

Because these servos have no feedback, `!jog` steps from the last *commanded*
angle rather than a measured one, and refuses when the position is unknown —
after a release, or before the servo has been seated with `!servo <name>
<angle>`. `!status` prints `?` rather than a number for those channels.

## Documentation

| Doc | What it covers |
|---|---|
| [docs/OPERATION.md](docs/OPERATION.md) | Day-to-day: power-up order, stopping, modes, network, what survives a reflash |
| [docs/CALIBRATION.md](docs/CALIBRATION.md) | Measuring the limits, the rules that stop it breaking parts, and the measured values |
| [docs/WIRING.md](docs/WIRING.md) | Pin map, power, `/OE`, bring-up order |
| [docs/decisions.md](docs/decisions.md) | Why things are the way they are |
| [docs/roadmap.md](docs/roadmap.md) | What is done and what is next |

## Calibrating

Full procedure in [docs/CALIBRATION.md](docs/CALIBRATION.md), including the
measured values for this build. The three rules that matter most, learned at the
cost of a broken lid arm:

1. **Seat the horn before measuring.** Servo at 90, lid just closed. Every lid
   done this way came out with a clean arc; every lid measured on its as-found
   horn ran out of travel and eventually slipped or broke.
2. **Confirm direction with one 2–5° step after any mechanical change.** A lid
   sits against its stop by definition, so the closing direction has no headroom
   — 10° the wrong way is 10° into the eye.
3. **The servo rail switch is the emergency stop.** `/OE` is unwired, so
   `!release` needs a live MCU and a working I²C bus. The switch does not.

Calibration survives reflashing. It does not survive erasing flash or a change to
`partitions.csv`.

## License

TBD.
