# Hardware

Everything on this page marked TODO is a placeholder in the firmware too.
Filling a value in here means changing it in code — grep for `TODO(hardware)`.

## Controller

Seeed Studio XIAO ESP32-S3 (variant: **TODO** — plain, Sense, or Plus?).

| Silk | GPIO | Assigned to | Notes |
|---|---|---|---|
| D0 | 1 | servo: pan | TODO confirm |
| D1 | 2 | servo: tilt | TODO confirm |
| D2 | 3 | servo: lid_upper_l | TODO confirm |
| D3 | 4 | servo: lid_upper_r | TODO confirm |
| D4 | 5 | servo: lid_lower_l | also default I²C SDA |
| D5 | 6 | servo: lid_lower_r | also default I²C SCL |
| D6 | 43 | *free* | UART TX — leave alone |
| D7 | 44 | *free* | UART RX — leave alone |
| D8 | 7 | *free* | SPI SCK |
| D9 | 8 | *free* | SPI MISO |
| D10 | 9 | *free* | SPI MOSI |

If I²C ends up needed (IMU, expander, gaze sensor), D4/D5 have to move and two
servos relocate to D8/D9.

## Mechanism

**TODO** — what is this, mechanically? Options discussed elsewhere:

- Nilheim Mechatronics style: printed frame, eyeballs on a shared gimbal, four
  lid servos, pushrods to horns.
- Ball-and-socket per eye with independent pan/tilt (more servos, more travel).
- Simplified 3-servo: pan, tilt, one lid pair.

The firmware currently assumes the first. Axis list lives in `eye_axis_t`.

## Servos

| Field | Value |
|---|---|
| Model | TODO |
| Count | 6 (assumed) |
| Stall current | TODO |
| Travel used | TODO — measured during calibration |

Placeholder pulse endpoints in `eye_servo_default_cal()` are 1000/1500/2000 µs.
Replace once measured; do not widen them speculatively.

## Power

**TODO.** The requirement, whatever the supply:

- Servos run from a dedicated 5–6 V rail, **not** the XIAO's 3V3.
- Grounds tied together between that rail and the board.
- Bulk capacitance across the servo rail (470–1000 µF) to absorb stall spikes.
- Six micro servos moving at once can pull several amps transiently — size the
  supply for stall, not for idle.

## Printed parts

TODO — link the CAD once it exists. Print settings, if relevant, belong here:
the build volume in play is 256 × 256 × 256 mm, and units are mm throughout.
