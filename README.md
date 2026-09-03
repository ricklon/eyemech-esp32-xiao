# eyemech-esp32-xiao

An animatronic eye mechanism on a Seeed Studio XIAO ESP32-S3 — servo motion,
autonomous idle behavior, and a browser control page for jogging and calibration.

> **Scaffold.** Structure and APIs are in place; nothing has been built or run on
> hardware yet, and the pin map, servo endpoints and axis list are placeholders
> marked `TODO(hardware)`. See [CLAUDE.md](CLAUDE.md) for the open questions.

## Quick start

1. Install [PlatformIO](https://platformio.org/) (VS Code extension or CLI).
2. `cp components/eye_web/include/secrets.h.example components/eye_web/include/secrets.h`
   and fill in your WiFi SSID and password.
3. Plug in the XIAO, then:

```
pio run -t upload
pio device monitor
```

The serial log prints the control page URL once WiFi associates:
`http://<ip>/` — or `http://eyemech.local/` if your network resolves mDNS.

## What's in the box

| Path | What it does |
|---|---|
| `src/main.c` | Boot sequence: NVS → servos → motion task → WiFi + HTTP |
| `components/eye_servo/` | LEDC 50 Hz servo driver, per-axis calibration persisted in NVS |
| `components/eye_motion/` | 50 Hz pose loop: easing, saccades, blinking, three modes |
| `components/eye_web/` | WiFi station, REST API, single-file embedded control page |
| `docs/hardware.md` | Pin map, BOM, power notes — fill in as the build firms up |
| `docs/roadmap.md` | Milestones and backlog |
| `docs/decisions.md` | Why things are the way they are |

## Control API

| Method | Path | Body |
|---|---|---|
| GET | `/api/state` | — |
| POST | `/api/mode` | `{"mode":"idle"\|"manual"\|"calibrate"}` |
| POST | `/api/look` | `{"x":-1..1,"y":-1..1,"speed":0..1}` |
| POST | `/api/lids` | `{"upper":0..1,"lower":0..1,"speed":0..1}` |
| POST | `/api/blink` | — |
| POST | `/api/jog` | `{"axis":"pan","us":1500}` — calibrate mode only |
| POST | `/api/cal` | `{"axis":"pan","min_us":1000,"center_us":1500,"max_us":2000,"inverted":false}` |
| POST | `/api/cal/save` | — commits calibration to NVS |

## Calibrating an axis

Servos strip their gears when driven past a mechanical stop, so work upward
carefully:

1. Switch to calibrate mode (button on the control page, or `POST /api/mode`).
2. Jog the axis in 25–50 µs steps from 1500 µs, watching the linkage.
3. Stop at the last position with no binding or buzz — that's your endpoint.
4. Repeat for the other direction, then set center.
5. `POST /api/cal` per axis, then **Save calibration to NVS**.

Calibration survives reflashing. It does not survive `pio run -t erase`.

## License

TBD.
