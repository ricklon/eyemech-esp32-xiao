# Calibrating the mechanism

Worked out on hardware, 2026-09-05, at the cost of one broken lid arm and one
slipped horn. The rules below are the ones that actually prevented damage; the
order matters.

## Why it works this way

These servos have **no feedback**. The PCA9685 is write-only PWM, nothing reports
position, load or current, and `eye_servo_read()` returns the last *commanded*
angle rather than a measurement. Two consequences shape everything here:

1. **Calibration is command-and-confirm.** The firmware commands a position, a
   human watches the linkage and says when to stop. This is not the
   capture-and-record flow used on servos that report position — the Feetech
   units in `~/Projects/lerobot`, say. Nothing from there ports.
2. **There is no stall detection.** A servo driven into a stop keeps pushing.
   These are MG90S: metal gears do not strip the way SG90 nylon does, so what
   gives instead is the horn, the linkage, or the printed part. Your eyes and
   ears are the only instrument.

## Safety

**The servo rail switch is the emergency stop.** `/OE` is pulled down and D10 is
not wired to it, so there is no hardware output-disable. `!release` works, but
over I²C — it needs a functioning bus and a live MCU. The switch needs neither.

**Leave `!safeboot on` until every axis is measured.** The board then comes up
with all channels released and sits in calibration mode. Without it, any reset
with the rail live drives all six channels to their stored angles within a
second — and a brownout on the servo rail *causes* a reset, which makes the
failure self-reinforcing.

**One servo at a time.** Nothing else should be driven while you work an axis.
`!engage` in calibration mode deliberately drives nothing for this reason.

## The rules that matter

**Seat the horn before measuring, not after.** With the servo at 90, fit the horn
so the lid is *just closed*. Every lid done this way came out with a clean
70–90° arc inside the servo's travel. Every lid measured on its as-found horn ran
out of travel at one end and eventually slipped or broke. This is the single
biggest difference between a calibration that works and one that costs parts.

**After any mechanical change, confirm direction with ONE 2–5° step.** Horn
orientation is not predictable by eye and is not consistent between lids —
measured on this build, TL opens *down* and TR opens *up*, for the same lid
function. A gaze axis has travel either side of centre, so a wrong 10° step is
recoverable. A lid sits against its stop by definition: the closing direction has
**zero headroom**, and 10° the wrong way is 10° into the eye or the frame. That
is exactly how the TL arm broke.

**Step sizes.** 5–10° while moving *away* from a stop; 2° for the last approach
to either end; a single small step whenever direction is unverified.

**Stop 2° short of a hard stop.** Mark the last position that did *not* bind, so
normal operation never presses it. UD's bottom is a hard stop at 40 and is marked
42 for this reason.

**Do not run a lid to the ends of the servo's range.** Below ~10 or above ~170
there is no margin left for a `trim_us` correction, and the pulse extremes are
where servos behave least predictably. A lid needing the full sweep is a horn
that wants re-indexing, not more travel.

## Procedure

Everything below is over the serial console — see `components/eye_console/`. It
needs no network, which is the point.

### Gaze axes (LR, UD)

1. `!mode calibration` then `!engage`. Nothing is driven yet.
2. `!servo LR 90` — seats the axis at a known position. **First motion; hand on
   the rail switch.**
3. `!jog LR +5` outward, watching the linkage. 2° for the last few degrees.
4. At the end of useful travel, `!mark LR max`. If it is a hard stop, back off
   2° first.
5. Return through centre and repeat for the other end, `!mark LR min`.
6. `!save`.

### Lids (TL, BL, TR, BR)

A lid's **closed** end is not a nominal angle — it is where the lid meets its
partner or the eye. It is also the value `eye_motion_blink_now()` drives to
several times a minute, so a couple of degrees past contact means a powered
grind on every blink. Hunt it; never assume it.

1. Fit the horn first: servo at 90, lid just closed.
2. `!engage` in calibration mode, then one small step to confirm which way opens.
3. Open in 5–10° steps to the open end. This is an **aesthetic** judgement, not a
   mechanical one — wide enough to read as alert, not so wide it shows mechanism.
   Comparing against the opposite lid's calibrated open position helps.
4. `!mark <lid> max`.
5. Park the *opposing* lid at its closed position, then creep this lid closed in
   2° steps until they **just touch**. `!mark <lid> min` at first contact.
6. `!save`.

Upper and lower lids on a side close against each other, so do them as a pair and
be aware the second one's closed value depends on where you put the first.

## Measured values

| Axis | Closed (`min`) | Open (`max`) | Arc | Notes |
|---|---|---|---|---|
| LR | 40 | 140 | 100° | Matches the reference table exactly |
| UD | 42 | 138 | 96° | Bottom is a hard stop at 40, backed off 2°. Top capped — see `decisions.md` |
| TL | 97 | 9 | 88° | Opens **downward**. Arm reprinted, horn reseated |
| TR | 89 | 165 | 76° | Opens **upward** |
| BL | 90 *(assumed)* | 160 | 70° | Closed never hunted |
| BR | 90 *(assumed)* | 20 | 70° | Closed never hunted |

Stored in NVS, namespace `eyemech`, key `servo_cal_v1`.

**Still outstanding:** BL and BR carry the nominal 90 for closed rather than a
measured value. Every lid actually hunted came out different from it — TL at 97,
TR at 89 — so 90 is an assumption in the place it matters most. BR closes against
TR, which is fully calibrated, so it can be done any time. BL closes against TL,
and note the circularity: TL's 97 was measured against BL sitting at the assumed
90, so if BL's real closed differs, TL inherited that error.

## Things that are not true, that look true

**Servo angles do not transfer between sides.** TR is `(89, 165)` and TL is
`(97, 9)` for the same lid function. Symmetry on this mechanism is a *physical*
property — how the two lids look — not a numeric one. Match the appearance and
let the numbers land where they land.

**Orientation is per-lid, not per-side.** A tidy theory that this build's left and
right were swapped relative to the reference fitted TR exactly, then BR
contradicted it. The horns are indexed independently. Measure each one.

**A lid that "moves but never fully opens"** is a horn indexed about 90° out: at
the flat end of the crank's arc, large servo rotation produces little linkage
travel, and it is near over-centre where it binds. Re-seat it rather than
chasing more travel.

**`!status` reports commanded angles, not measured ones.** A channel showing
`90.0` while the servo rail is off is still reporting 90.0. `?` means the
position is genuinely unknown — before the first write, after a release, or on a
channel the boot readback found idle.
