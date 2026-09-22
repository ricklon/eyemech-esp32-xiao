# Standard operating procedure

Day-to-day running of the mechanism. Calibration is a separate job — see
[CALIBRATION.md](CALIBRATION.md).

## Power up

Order matters, and the reason is that the ESP32 and the PCA9685's logic run off
USB/3V3 while the servos run off a separate rail.

1. **Logic first.** USB to the XIAO. The board boots, brings up I²C, WiFi and the
   console. With `safeboot on` it comes up **released** — every channel off,
   mode `standby`, nothing driven.
2. **Check it came up clean.** `!status` over the serial console at 115200, or
   watch the boot log. Expect `pca9685: ready at 0x40`.
3. **Servo rail second.** Nothing should move — not a twitch. If anything does,
   the outputs are not gated and something is wrong; kill it.
4. **`!engage`** when you actually want it live. In `standby` or `calibration`
   this drives nothing; in any other mode it drives to neutral, which *is* a
   movement.
5. **`!mode auto`** (or `tracking` with a Grove Vision module attached). Leaving
   standby drives every servo to neutral at full speed: after a release there is
   no known position to ease from.

Once every axis is calibrated, `!safeboot off` makes it come up running instead,
which is what you want for a demo. Turn it back on before any mechanical work.

## Stopping

| Situation | Do this |
|---|---|
| Normal stop | `!release` — all channels off, **latched**. Nothing moves again, blinks included, until `!engage` |
| Something is binding or buzzing | **Cut the servo rail.** Immediate, and needs no working firmware or I²C bus |
| Mechanical work | `!release`, then cut the rail. Limp is what you want for fitting horns |

There is **no hardware emergency stop**: `/OE` is pulled down and D10 is unwired,
so `!release` goes over I²C and depends on a live MCU and a working bus. The rail
switch does not. Treat the switch as the real e-stop.

## Modes

| Mode | Behaviour |
|---|---|
| `tracking` | Follows the Grove Vision module; blinks on a wall-clock timer |
| `auto` | Random gaze and blink. The default with no vision module |
| `manual` | `!look <lr> <ud>` or the web page drives the gaze |
| `calibration` | Everything to 90 once on entry, then **nothing per tick** — direct writes stick. The only mode where `!servo` / `!jog` / `!mark` are accepted. Entered from `standby` it seeds nothing, so axes still come up one at a time |
| `standby` | Stopped: nothing driven on entry, nothing per tick, no blinks, no animations, and the MCP motion tools refuse. Where safe boot lands. Leaving it takes a person |
| `follow` | Eases toward live poses — gaze plus one value per lid, or one per eye — from `/ws/pose`, `POST /api/pose` or `!pose`, at most 300°/s for gaze and 600°/s for lids. No timer blinks. Entered by sending a pose (from auto, manual or tracking), not by `!mode`. One second without a pose, or `!follow stop`, eases back to neutral and returns to the previous mode. Browsers may open `/ws/pose` from the board's own pages or from `CONFIG_EYE_WEB_EXTRA_ORIGINS`, by default the eye-tracking dashboard at `http://localhost:8080` |

Every other mode change runs `neutral()` and clears a half-finished blink.

## Console

115200 over USB. `!help` lists everything. The console needs no network, which is
why it exists — WiFi is not a dependency for stopping the mechanism.

```
!status                 mode, release latch, vision, trim, heap, servo table
!release / !engage      stop (latched), and the way back
!mode <name>            tracking | auto | manual | calibration | standby
!blink                  queue one blink
!trim <0..1>            lid openness, scaled within the calibrated limits
!look <lr> <ud>         manual gaze in degrees
!wifi                   link, SSID, IP, access point
!safeboot [on|off]      boot released, or boot running
!reboot
```

## Network

The recovery access point is **always up**, in every station state. If no station
profile connects, join `eyemech-setup` and browse to <http://192.168.9.1/>.
Otherwise the board answers to `eyemech.local` over mDNS on either interface.

Changing networks never needs a reflash:

```
!wifi scan
!wifi set "Some Network" thepassword
```

`!wifi set` joins first and saves second. The credentials are tried live, and
they are written to a profile only once the station reaches an IP — a wrong
password reports `nothing saved`, leaves the profile list untouched, and drops
back to whatever network was working. Expect it to take up to half a minute to
say so: a refused association costs several seconds and there are three retries
behind the first attempt.

The four profiles are a FIFO, so nothing has to name a slot. A new network takes
a free one, and evicts the least recently joined when all four are full; joining
a network already on the list refreshes it in place instead of consuming a second
slot. `!wifi list` names the entry the next join would replace once the list is
full. `!wifi set <n> <ssid> <password>` still writes slot `n` directly, without
trying the credentials — that is the one path where a typo can be stored.

Profiles are swept in turn — three retries each, then the list is re-checked
every two minutes while parked on the access point. Only a profile that actually
obtained an IP becomes the boot default, so a typo does not survive a power
cycle.

## What survives what

| Event | Calibration | WiFi profiles | Safe boot flag |
|---|---|---|---|
| Power cycle | kept | kept | kept |
| `!reboot` | kept | kept | kept |
| Reflash (`pio run -t upload`) | kept | kept | kept |
| `erase_flash` | **lost** | **lost** | **lost** |
| Changing `partitions.csv` | **lost** | **lost** | **lost** |

All of it lives in NVS. Note the last row: adding OTA partitions is on the
roadmap and would wipe a calibration that cost real bench time. Export or record
the values first — `!status` prints the table.

## After a brownout or crash

The servo rail dying while the board runs is **invisible to the firmware**. The
PCA9685 keeps its registers and keeps commanding; `!status` goes on reporting the
last commanded angles even though the mechanism may have sagged. With no feedback
this is unavoidable.

On a board reset it is different and better: `pca9685_get_us()` reads the LEDn
registers back, so a *warm* reset recovers the last commanded position and eases
to neutral over 600 ms instead of jumping. A cold start recovers nothing — the
registers are zeroed by the chip's own reset — and goes straight to neutral.

If a reset happens with the rail live and `safeboot off`, the mechanism will move
on its own within a second of coming back. That is the case `safeboot on` exists
to prevent.
