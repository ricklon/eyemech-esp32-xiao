# Decision log

Short entries. Date, decision, reasoning, and what would change our mind.

## 2026-09-03 — Port MicroPython to ESP-IDF C, keeping the original

The MicroPython build works. The port is for headroom — WiFi, a real control
surface, and eventually sharing a codebase with the xiaozhi voice agent, which
is ESP-IDF native.

`micropython/` stays in the repo as the reference implementation rather than
being deleted. When the two disagree, the Python is right until a decision here
says otherwise. Deleting it would throw away the only known-good description of
how the mechanism should behave.

## 2026-09-03 — Both XIAO boards, one codebase

Targets the ESP32-C6 (what it runs on today) and the ESP32-S3 (more headroom,
the xiaozhi path, a camera on the Sense variant). All pin differences live in
`components/board/include/board_pins.h`; no other file contains a GPIO number.

The cost is one indirection. The benefit is that choosing a board later is a
build-flag change rather than a port.

Would revisit if: a third board needs peripherals the abstraction can't express.

## 2026-09-03 — Keep the PCA9685 rather than driving servos from the MCU

The hardware exists and is wired. Native LEDC would save a part and an I²C bus,
but it would also give up `/OE` — the one mechanism that reliably keeps servos
limp through a reset — and would rewire the board.

Would revisit if: I²C latency turns out to cap the motion loop somewhere that
matters.

## 2026-09-03 — Web control replaces the pots and switches

Three ADC pots, an enable switch, a mode switch and a blink button became a WiFi
control page. Only GPIO0/1/2 are ADC-capable on the C6, so the pots had the pin
map pinned around them; dropping them frees D0–D3, D8 and D9 on both boards, and
calibration stops requiring a reflash.

The loss is real: a control page needs a network and a phone, where a pot needs
neither. If bench work turns out to want physical controls back, the freed pins
are still there and the abstraction to re-add them is `eye_web`'s API surface.

## 2026-09-03 — Calibration persists in NVS

The MicroPython build edited `servo_limits` at the top of `main.py` and
reflashed. With calibration now editable from a browser, it has to survive a
reboot, so limits and per-servo pulse config are stored as one blob under
namespace `eyemech`.

One blob rather than per-axis keys so a partial write can't leave axes
inconsistent.

## 2026-09-03 — Angles, not normalized units

Everything above `eye_servo` speaks in servo degrees, exactly as the Python did.
A normalized -1..+1 API would be tidier, but it would make every formula in
`control_ud_and_lids()` differ from the reference, which is the one thing that
makes the port checkable.

Would revisit if: the mechanism is redesigned and the reference stops mattering.

## 2026-09-05 — The left lid pair is mirrored as an assembly

`TL` measured `(128, 20)` and `BL` measured `(40, 170)`. Both are the *opposite*
orientation to the reference table, which has TL conventional `(90, 170)` and BL
inverted `(90, 10)`.

Two lids each coming out backwards is not two coincidences. The left lid pair is
mounted mirrored as a unit relative to Will Cogley's build. Channel identity was
confirmed at the bench for both — ch2 drives the physical top-left lid, ch3 the
bottom-left — so this is orientation, not a swapped channel map.

Prediction for the right pair, to be tested rather than assumed: the table has
`TR` inverted and `BR` conventional, so expect `TR` conventional (opens high) and
`BR` inverted (opens low).

Nothing in the code cares. `clamp_to_limits()` and `lid_open()` both use
`fminf`/`fmaxf` and never assume an ordering, which is exactly why the table
entry is written as (closed, open) rather than (low, high).

Separately, both left lids use over 100° of servo sweep — TL 108°, BL 130° — for
an arc that should take 60–90°, and both run out of room at one end. That points
at the horns being indexed a few splines off. Worth re-seating before the
calibration is treated as final: it would put each arc in the middle of the
servo's travel, leaving margin at both ends and making `trim_us` useful instead
of already spent.

## 2026-09-05 — TL is mirrored on this build, and closes at 128 not 90

Measured on hardware. The reference table has `TL = (90, 170)`: closed at 90,
opening as the angle rises, with only `BL` and `TR` running backwards. On this
mechanism TL runs backwards too. Measured value is `(128, 20)` — closed at 128,
wide open at 20.

So **three** of the four lids are mirrored here, not two. The channel map is not
at fault: channel 2 was confirmed at the bench to drive the physical top-left
lid. It is a mounting inversion on that servo.

The code needed no change. `clamp_to_limits()` uses `fminf`/`fmaxf`, and
`lid_open()` interpolates from `min` toward `max` without assuming an ordering,
so an inverted pair works exactly as the existing BL/TR entries do. This is the
payoff for the standing rule against "normalizing" that table.

The closed value matters more than the inversion. An early reading suggested TL
was closed at 160; hunting for the *first* angle that shuts the lid found 128.
`eye_motion_blink_now()` drives to `min` on every blink, several times a minute,
so a `min` of 160 would have pressed 32° past shut into the eyeball on every one
of them — silently, since MG90S have the torque to keep pushing and nothing here
reports load.

Consequence for the remaining lids: the reference's closed value of 90 is not
trustworthy on this build. Each lid gets its closed end hunted for, not assumed.

## 2026-09-05 — UD capped at 138 rather than its true limit

Measured on the real linkage: UD's travel is asymmetric. The bottom is a hard
stop at 40 (marked 42, two degrees clear so normal use never presses it), while
the top was still moving freely past 150 — and level gaze sits at servo 90, so
this is genuine geometry, not a horn fitted a spline off. `trim_us` stays 0.

Capped `max` at 138 anyway, giving 48° each way about 90.

`control_ud_and_lids()` derives `progress` as `(ud - min) / (max - min)`, and the
four lids track it. The reference table's 40/140 puts level gaze at exactly
`progress` 0.5. Taking the full 42/150 would move level gaze to 0.444 and leave
the upper lids around 4% more hooded at rest — a permanent change to the resting
face, which is the thing the 0.8 / 0.4 coefficients exist to protect.

The cost is 12° of proven upward travel, which is real: up-gaze is expressive.

Would revisit by giving each axis an explicit neutral angle, so `progress` is
computed about the true mechanical centre instead of the range midpoint. That
buys full travel *and* the correct resting face, but it changes a formula that
currently matches the reference line for line, which is the property that makes
the port checkable at all. Not worth doing mid-calibration.

## 2026-09-05 — Lid trim scales within the calibration instead of replacing it

`update_eyelid_limits()` in the original rewrote the four lid entries of
`servo_limits` from hardcoded ranges every time the trim pot moved. In the
MicroPython build that was harmless: the pot was the *only* source of lid
limits.

In the port it is not. `servo_limits` is now also the calibration table that
`eye_servo_save()` persists, so the trim writing into it meant one drag of the
openness slider silently replaced measured endpoints with hardcoded numbers,
and the next **Save to NVS** committed the damage. Calibrate the lids, touch the
slider, save, and the linkage measurements were gone.

So the trim is now a stored 0..1 that a `lid_open()` helper applies:

    open = min + (max - min) * (0.5 + 0.5 * trim)

Limits belong to calibration; openness scales within them. `control_ud_and_lids()`
takes its open endpoint from `lid_open()` rather than `.max`; substituting `.max`
back recovers the original expressions exactly, and the 0.8 / 0.4 coefficients
are untouched.

On the default table this reproduces the pot's numbers exactly for TL, BL and
TR at every trim setting. **BR differs** — 125° rather than 130° at trim 0, and
160° rather than 170° at trim 1 — because the original's BR trim range of
130–170 ran past BR's own declared limit of `(90, 160)`. The two were already
inconsistent in the reference; this resolves it in favour of the declared limit,
on the grounds that a trim control should not be able to drive a servo past the
endpoint calibration says is its mechanical stop.

Would revisit if: BR turns out to need more travel than 160° on the real
linkage, in which case the fix is to recalibrate BR's limit, not to let the trim
overrun it again.

## 2026-09-03 — `servo_limits` may run backwards, on purpose

`BL` and `TR` are `(90, 10)`: max below min, encoding that those lid servos are
mounted mirrored. Clamping code uses `fminf`/`fmaxf` rather than assuming an
ordering. Normalizing the table would silently invert two lids.
