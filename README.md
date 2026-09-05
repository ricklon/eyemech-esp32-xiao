# eyemech-esp32-xiao

Will Cogley's animatronic eye mechanism on a Seeed XIAO — six servos on a
PCA9685, optional face tracking from a Grove Vision AI module, and a browser
control page.

Two implementations live here:

- **`micropython/`** — the working original. XIAO ESP32-C6, MicroPython, three
  files copied to the board. This runs today.
- **everything else** — an ESP-IDF C port targeting both the C6 and the S3.
  Scaffolded, not yet compiled. This is where new work goes.

> The port is not built or bench-tested yet. Calibration constants are the
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
cp components/eye_web/include/secrets.h.example components/eye_web/include/secrets.h
# fill in WiFi credentials, then:
pio run -e xiao_esp32c6 -t upload
pio device monitor
```

The serial log prints the control page URL once WiFi associates.

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

## Calibrating

Servos strip their gears when driven past a mechanical stop, so work up to the
endpoints rather than guessing at them:

1. Switch to calibration mode. Everything goes to 90°.
2. Fit horns and linkages with everything at 90°.
3. Move one servo at a time in small steps, watching the linkage. Stop at the
   last position with no binding or buzz — that is the endpoint.
4. Enter the min/max into the servo table, then **Save to NVS**.
5. Use `trim_us` for mechanical centring offsets rather than fudging the limits.

Calibration survives reflashing. It does not survive erasing flash.

## License

TBD.
