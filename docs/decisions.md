# Decision log

Short entries. Date, decision, reasoning, and what would change our mind.

## 2026-09-03 — ESP-IDF over Arduino

Chose PlatformIO + ESP-IDF rather than PlatformIO + Arduino.

The eye mechanism may eventually share a board or a codebase with the xiaozhi
voice agent, which is ESP-IDF native. Starting on Arduino would mean either a
port later or a permanent bridge layer. The cost is a slower first blink and
more boilerplate around WiFi and HTTP.

Would revisit if: the xiaozhi integration is dropped and the project stays a
standalone eye mechanism forever.

## 2026-09-03 — LEDC rather than MCPWM for servo PWM

LEDC gives eight channels off one 50 Hz timer with a trivial API, and servo
timing tolerance is generous — jitter of a few microseconds is invisible.

Would revisit if: an axis needs tight phase relationships with another, or the
channel count outgrows eight.

## 2026-09-03 — Normalized units above the servo layer

Everything above `eye_servo` speaks in -1..+1 (lids 0..1). Microseconds live in
the driver and the calibration endpoints only.

Keeps mechanical differences between linkages — different horn lengths, mirrored
lid servos, inverted mounting — out of the motion logic entirely. The `inverted`
flag in the calibration struct absorbs mirroring rather than the motion code
special-casing left vs. right.

## 2026-09-03 — Single-file embedded control page

`index.html` is embedded with `EMBED_TXTFILES`, no build step and no external
requests. The board may well end up on a network with no internet route, and a
control page that needs a CDN is a control page that fails when you need it.
