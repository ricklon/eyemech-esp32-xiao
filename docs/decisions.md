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

## 2026-09-17 — An MCP server on the board, without engage or calibration

`POST /mcp` in `eye_web` makes the mechanism an MCP server that any client can
reach by URL, with no hub or helper process in between. The alternatives were
porting onto xiaozhi-esp32 (an application with a voice pipeline this board does
not have, in C++, and it would abandon the calibration model that works here),
a host-side proxy over the HTTP API (quick, but only reachable from that host),
and registering with agent-hub as a robot (the eventual route to eyes that react
to a voice agent, which can reuse the same tool table).

The server speaks MCP 2026-07-28, which is stateless, and also answers the
older `initialize` handshake (2025-03-26 to 2025-11-25). It does both because
it is not yet known which revision a given client speaks. Every answer is a
single JSON body: no SSE, no sessions, and nothing a microcontroller has to keep
open.

The tools are get_state, look, blink, play_animation, stop_animation, set_mode
and release. **Engage and everything used for calibration are deliberately
missing.** A release is the software stop, and undoing it must take a person at
the console or control page, not a model. Calibration is command-and-confirm
with a human watching a mechanism that reports nothing back, and the broken lid
arm is what skipping that costs. The motion tools also refuse while released or
in calibration mode, so a board in safe boot cannot be moved over MCP at all.

Gaze is 0..1 across each axis's calibrated range rather than degrees, for the
same reason animation keyframes are: it survives recalibration and axes whose
limits run backwards.

There is no authentication, the same as the rest of the HTTP API. The Origin
check stops a web page in a browser from driving it, and that is all.

**Checked 2026-09-17:** Claude Code 2.1.274 speaks the modern revision. A
headless `claude -p` run called `get_state` through the registered server, and
`/api/state`'s `mcp_last` recorded era `modern`, version `2026-07-28`, client
`claude-code 2.1.274` and status 200, with no `initialize`. The legacy path stays
for other clients; nothing here depends on it.

Would revisit if: the board leaves a trusted LAN (add auth first), or a client
needs change notifications (`subscriptions/listen`), which means holding a
socket open.

## 2026-09-17 — A standby mode, separate from calibration

Safe boot used to land in `calibration` because it was the only mode whose loop
drives nothing. That made one label mean two things — "stopped, waiting for a
person" and "fitting horns and measuring limits" — and after calibration was
finished the board still announced itself as calibrating on every reboot.

`standby` is the stopped state: nothing driven on entry, nothing per tick, no
blinks, animations refused, and the MCP motion tools refuse. Safe boot lands
there. `!engage` in standby drives nothing, as it does in calibration.
The MicroPython original has no such mode. Its enable switch did not stop
anything: it selected pot control ("controller"), and the mode switch forced
calibration, so the closest it had to stopped was calibration too.

Entering calibration from standby seeds nothing, where from a running mode it
still drives all six to 90. Without that, engaging in standby and then choosing
calibration would move every servo at once, which is the one thing calibration's
one-axis-at-a-time procedure exists to prevent.

Leaving standby for a running mode drives to neutral at full speed. After a
release no position is known, so there is nothing to ramp from.

Would revisit if: a slow first move out of standby turns out to matter enough to
build — for example ramping each servo from its neutral in turn.

## 2026-09-17 — Follow mode for live poses from a tracker

The sibling eye-tracking project measures a face and wants the mechanism to copy
it. Follow mode takes a pose — `lr`, `ud` and one value per lid servo (`lid_tl`,
`lid_bl`, `lid_tr`, `lid_br`), each 0..1 — over a WebSocket (`/ws/pose`), plain
HTTP (`/api/pose`) or the console (`!pose`).

- **Four lids, with a paired shorthand.** eye-tracking's controller already
  produces upper and lower openness per eye, with the lower lid moving less until
  nearly shut, and the mechanism has four servos. `lid_l`/`lid_r` set both lids
  of an eye together for senders that have one value; mixing the two forms is
  refused. Each lid stops at its own calibrated closed end, measured with the lids
  meeting, so both closed is the calibrated closure. Watched on the mechanism
  2026-09-17: blinks every 3 s through all four lids at the 600°/s lid rate, then
  all four closed and held for 3 s — both eyes closed, no knock, no stall buzz.

- **Every field is required.** An animation frame can leave a lid NAN to hand it
  to the 0.8/0.4 gaze coupling; a pose cannot. This copies a measured face, whose
  lids already do whatever they do, so the coupling does not apply while following.
- **Lid 1.0 is the trimmed open position**, what neutral uses, not the calibrated
  maximum. Animation frames use the calibrated range; a tracker's "fully open"
  should read as this face's normal open, not wider.
- **Left and right are the mechanism's** (`lid_l` is TL/BL). Mirroring a camera
  image is the sender's decision, made once, there.
- **The motion task applies poses, rate-limited**: 300°/s gaze, 600°/s lids. The
  web task only stores the latest pose under a lock. Nothing else in the firmware
  limits speed; this does because a tracker's output jumps.
- **No timer blinks while following**: the sender's lids are the blinks.
- **Entered without neutral()**, the way animations are, so starting to follow
  does not jump. Poses are refused while released or in standby, calibration or
  an animation. Animations and `!mode follow` are refused while following.
- **One second without a pose eases to neutral at the same rates, then returns**
  to the previous mode directly — also without the full-speed neutral() a mode
  change runs. `!follow stop`, `POST /api/follow/stop` or `{"stop":true}` on the
  socket does the same at once.
- **The WebSocket refuses a cross-origin browser before the upgrade**, via the
  pre-handshake callback, for the same reason `/mcp` checks Origin. A browser
  tracker therefore reaches the board through a local bridge, which it needs
  anyway: a camera needs HTTPS or localhost, and the board serves plain HTTP.

**A pose sender must set `TCP_NODELAY`.** Tested on the mechanism 2026-09-17: the
same 30 Hz stream was visibly choppy until `tools/posetest.py` disabled Nagle's
algorithm, and reasonably smooth after, with no firmware change. Nagle holds each
small frame back waiting for an ACK, so poses arrive in bunches. Browsers disable
it for WebSockets; a Python or other native bridge has to do it explicitly.

The rates and timeout are compile-time constants in `eye_motion.h`, chosen, not
measured. Would revisit if: tracked blinks look sluggish (raise the lid rate), a
real stream jitters visibly (smoothing belongs in the sender, which has the
timestamps), or the lid rates need to differ between upper and lower lids.

## 2026-09-17 — Automatic blinks: adjustable gap, and an alternating-wink style

Watching the rebuilt mechanism in manual mode, the reference cadence — a blink
of all four lids every 2–7 s, from `micropython/main.py` — read as repetitive
and fast. Both the gap and what an automatic blink does are now settings, saved
with Save, and both default to the reference, so nothing changes until someone
chooses to.

- **Gap:** a random pause drawn from min..max, 1–60 s.
- **Style:** `both` (the reference) or `alternate`, which winks one eye and then
  the other next time, back and forth.
- **Only automatic blinks follow the style**: the timer's, and the ones auto mode
  queues. A blink asked for by name — the page's Blink button, `!blink`, the MCP
  `blink` tool — is always both eyes, because that is what was asked for.

Alternating winks are a real change to how the face reads, not a tweak, which is
why the reference stays the default rather than being replaced.

Would revisit if: alternate turns out to be the keeper, in which case make it the
default and say so here.

## 2026-09-17 — Chosen on this mechanism: alternating winks, 10–20 s, 150 ms hold

Set at the bench and saved to NVS:

- **Blink style `alternate`**: winks that alternate eyes, rather than all four
  lids. Watched working: left and right eyes took turns, the other eye staying open.
- **Blink gap 10–20 s**: the reference 2–7 s read as a repeated fast blink.
- **Blink hold 150 ms**: the reference 70 ms reopens before these lids meet.

They survived the board being unplugged and plugged back in: /api/state reported
all three after the power cycle. Like the calibrated limits, they live in NVS,
not in the source. The compiled
defaults are still the reference (both, 2–7 s, 70 ms), so a board with erased
NVS comes up blinking the original way until they are set again.

Would revisit if: alternate survives a longer run without reading as a tic, in
which case it becomes the default in the source, per the entry above.

## 2026-09-21 — A stdio proxy in front of `/mcp`, so an absent board is not a dead server

Registered by URL, Claude Code probes `http://eyemech.local/mcp` once at session
start with a 5 s limit. With the board unplugged, resolving `eyemech.local` over
mDNS alone takes 5.1 s to fail on this host, so the probe times out, the server
is marked failed, and its tools stay missing for the rest of the session even
after the board comes back.

`tools/eyemech_mcp.py` is a stdio MCP server that Claude Code launches locally.
It forwards every request to `POST /mcp` unchanged, deriving the transport
headers from the body. It caches the replies that only change with a reflash
(`initialize`, `server/discover`, `tools/list`), answers those from the cache at
once and refreshes them in the background. A tool call made while the board is
away comes back as a tool error saying so; the next call after it returns
reaches it. It also remembers the board's IP address after the first answer, so
the mDNS lookup is not paid on every call.

This walks back part of the 2026-09-17 entry, which rejected a host-side proxy.
That rejection still holds for the thing it was about: the board is still a
complete MCP server that any client can reach by URL, and the proxy adds no
tools, no state and no protocol logic of its own. It exists only because this
client cannot tolerate the server being absent at startup.

Over stdio, Claude Code 2.1.278 opens with the legacy `initialize` at
2025-11-25, not the stateless revision it uses over HTTP, so the board's legacy
path is now the one in daily use from this host.

**Checked 2026-09-22** against the board on 192.168.1.212, running
`fix/mcp-version-header`: `initialize`, `tools/list` and a `get_state` call all
go through the proxy, and `mcp_last` recorded era `legacy`, version `2025-11-25`,
client `claude-code 2.1.278`, `tools/call`, 200. `claude mcp list` shows the
server connected. Pointed at an unreachable host with the cache in place,
startup is still answered at once and a `look` comes back as a tool error
without reaching the board.

The cache needs the board to have answered once. With no cache and no board,
`initialize` fails and the server shows as failed, just as the URL registration
did.

Would revisit if: Claude Code retries failed servers on its own, or the board
moves behind agent-hub, which would own this problem instead.
