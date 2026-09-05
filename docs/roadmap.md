# Roadmap

## M1 — Get the port compiling and running (current)

- [x] Repo restructured: MicroPython original preserved as reference
- [x] `pca9685`, `eye_servo`, `eye_motion`, `eye_vision`, `eye_web` scaffolded
- [x] Board abstraction so C6 and S3 both build
- [x] **First build.** Both boards, ESP-IDF 5.5.1 via PlatformIO
- [x] Bench test: PCA9685 answers on I²C at 0x40 (S3, servo rail off)
- [x] Serial console, WiFi profile sweep, AP fallback, mDNS, NVS round-trip
- [x] Warm-boot position recovery from the PCA9685 registers
- [x] `/OE`: **D10 is not wired.** The pull-down holds outputs enabled, so
      `!release` works entirely through the I²C `all_off()` path. There is no
      hardware stop; the servo rail switch is the emergency stop
- [x] Servo rail powered on an assembled mechanism with every channel released
      — nothing moved, confirming the outputs really are gated
- [x] LR (channel 0) centres on command and holds quietly
- [x] **LR calibrated: 40 / 140, matching the reference table**, measured by
      jogging to each end on the real linkage. Saved to NVS
- [x] **UD calibrated: 42 / 138.** Bottom is a real hard stop at 40, backed off
      2°. Top was still free past 150, but capped at 138 to keep level gaze at
      `progress` 0.5 — see docs/decisions.md
- [x] Servos identified: **MG90S**
- [x] **TL calibrated: 128 / 20.** Mirrored on this build, unlike the reference
      — so three lids run backwards here, not two. Closed is 128, not the
      table's 90; see docs/decisions.md
- [x] **BL calibrated: 40 / 170**, conventional — the reference has it inverted.
      Closed is where it meets the top lid, found with TL held closed
- [ ] **Blocked: TL lid arm broken 2026-09-05, needs reprinting.** Broken by
      jogging 10° steps in the closing direction after a horn refit, before
      direction had been confirmed. See the bring-up rule in docs/WIRING.md
- [x] Re-index the left lid horns — done, closed now lands at the design's 90
      rather than 128, giving a correct 80° arc with margin at both ends
- [ ] Re-measure TL open end once the arm is reprinted (was reading 10–20)
- [ ] BL: re-measure from scratch; its horn slipped during the first pass
- [ ] TR, BR — not yet started
- [ ] `!safeboot off` once all six are calibrated
- [ ] Side-by-side against the MicroPython build: same motion, same blink feel

## M2 — Behavior parity and beyond

- [ ] Verify `control_ud_and_lids()` reads identically to the Python
- [ ] Grove Vision tracking working in C — the parser is the risky part
- [ ] Blink timing with natural variance rather than uniform random
- [ ] Named expressions (neutral, alert, sleepy, squint) as pose presets
- [ ] Smooth pursuit vs. saccade as distinct motion profiles

## M3 — Integration

- [ ] Decide the xiaozhi relationship: eyes driven by voice-agent state
      (listening / thinking / speaking) rather than by a camera
- [ ] A command protocol for that — WebSocket or MQTT
- [ ] OTA updates, so the mechanism doesn't need disassembly to reflash

## Open on hardware

- The S3 is the board with bench time. The C6 builds but has not been run.
- LR's reference limits proved correct on the real linkage, which is decent
  evidence the rest of the table is a sound starting point rather than a guess.
  It is not evidence that any *other* axis is right.
- Servo type is still unrecorded (SG90 vs MG90S changes pulse range and supply
  sizing), and `min_us`/`max_us` are still the 500–2500 defaults — untested,
  since LR reached both reference endpoints without needing them widened.
- `pca9685_trim_oscillator()` is still dead code: the oscillator has not been
  measured, and there is no way to apply a measurement without a reflash.

## Deferred

- Replace the Grove Vision string-scraping with a real JSON parse
- Second mechanism synchronized over the network
- Servo current sensing as a stall detector
