# CLAUDE.md — eyemech-esp32-xiao

Guidance for Claude Code working in this repo. Keep it current: when a decision
here turns out wrong, fix the line rather than working around it.

## What this is

An animatronic eye mechanism driven by a Seeed Studio XIAO ESP32-S3. Firmware is
ESP-IDF, built through PlatformIO. The first milestone is servo motion plus a
WiFi control page for jogging axes and saving calibration.

**Status: scaffold.** Nothing here has been compiled or run on hardware yet. The
structure and APIs are deliberate; the numbers are placeholders. Every guess is
marked `TODO(hardware)` or `TODO(tuning)` — treat those as blocking questions for
Rick, not as invitations to invent values.

## Board facts

- **MCU**: ESP32-S3R8, dual-core Xtensa LX7 @ 240 MHz, 8 MB flash, 8 MB PSRAM
- **Silk → GPIO**: `D0=1 D1=2 D2=3 D3=4 D4=5 D5=6 D6=43(TX) D7=44(RX) D8=7 D9=8 D10=9`
- **Servo PWM**: LEDC, low-speed mode only on the S3. One timer at 50 Hz,
  14-bit resolution, one channel per axis. 8 channels available, 6 in use.
- **Do not** reassign D6/D7 (USB-serial console) without saying so explicitly.
- Servos are **not** powered from the XIAO's 3V3 rail. External 5–6 V supply,
  common ground with the board. See `docs/hardware.md`.

## Build and flash

Builds run on Windows, where PlatformIO lives — not inside any Linux sandbox.

```
pio run                  # build
pio run -t upload        # flash
pio device monitor       # serial, 115200
pio run -t menuconfig    # ESP-IDF config; promote keepers to sdkconfig.defaults
pio run -t clean
```

`sdkconfig` is generated and gitignored. `sdkconfig.defaults` is the checked-in
source of truth.

Before the first build, copy `components/eye_web/include/secrets.h.example` to
`secrets.h` in the same directory and fill in WiFi credentials. That file is
gitignored — never commit it, never paste its contents into a commit message,
an issue, or a doc.

## Layout

```
src/main.c                  boot sequence only — keep it thin
components/eye_servo/       LEDC servo driver + per-axis calibration in NVS
components/eye_motion/      pose, easing, idle saccades and blinking (50 Hz task)
components/eye_web/         WiFi STA + esp_http_server + embedded control page
docs/                       hardware notes, roadmap, decision log
```

**Layering rule**: `eye_web` → `eye_motion` → `eye_servo`. Never skip a layer.
The one sanctioned exception is calibration, where the web layer reaches
`eye_servo_set_us()` directly, and only while mode is `EYE_MODE_CALIBRATE`.

## Conventions

- C, not C++. ESP-IDF style: `esp_err_t` returns, `ESP_RETURN_ON_ERROR` /
  `ESP_ERROR_CHECK` at boundaries, one `TAG` per file.
- Public API in `include/<component>.h`; everything else `static`.
- Normalized units (`-1.0 .. +1.0`, lids `0.0 .. 1.0`) everywhere above the
  servo driver. Microseconds appear only inside `eye_servo` and the
  calibration endpoints.
- Calibration is the only place pulse widths get clamped. Clamp, never wrap.
- The web page is embedded via `EMBED_TXTFILES` — edit
  `components/eye_web/index.html` directly, no build step, no CDN, no bundler.
  It must stay a single file with no external requests.

## Safety rails that matter

A servo commanded past its mechanical limit stalls, heats, and strips gears
within seconds. So:

- `EYE_SERVO_PULSE_MIN_US` / `MAX_US` are absolute; nothing writes outside them.
- Calibration setters reject `min >= max` and centers outside the range.
- Anything that widens travel needs a bench test before it lands.
- On any new axis, jog in **small** steps and watch the mechanism.

## Open questions for Rick

These block real values in the code. Ask rather than assume:

1. How many servos, and which axes? The code assumes six: pan, tilt, and four
   independent lids. A 4-servo build drops the lower lids; a 3-servo build
   ties the lids together.
2. Mechanism type — printed Nilheim-style linkages, a ball-and-socket gimbal,
   something of your own? This decides whether pan/tilt are independent.
3. Servo model (SG90, MG90S, something with metal gears?) — sets the real
   pulse endpoints and current budget.
4. Power: what's feeding the servos, and how much headroom?
5. Does this eventually merge with the xiaozhi voice agent, and if so does the
   eye firmware drive itself or take commands from the agent?

## Working style

- Small commits, one concern each.
- If a change can't be verified without hardware, say so in the commit body.
- Don't add dependencies without asking; ESP-IDF's built-in components cover
  everything planned so far.
