# Roadmap

## M1 — Get the port compiling and running (current)

- [x] Repo restructured: MicroPython original preserved as reference
- [x] `pca9685`, `eye_servo`, `eye_motion`, `eye_vision`, `eye_web` scaffolded
- [x] Board abstraction so C6 and S3 both build
- [x] **First build.** Both boards, ESP-IDF 5.5.1 via PlatformIO
- [x] Bench test: PCA9685 answers on I²C at 0x40 (S3, servo rail off)
- [x] Serial console, WiFi profile sweep, AP fallback, mDNS, NVS round-trip
- [x] Warm-boot position recovery from the PCA9685 registers
- [ ] `/OE`: confirm D10 is wired to it, and that driving it high really does
      stop the outputs. The GPIO write succeeds; the wire is unverified
- [ ] One servo on channel 0, unloaded, centres on command
- [ ] All six servos, calibrate limits with `!jog` / `!mark`, save to NVS
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
- No servo has been powered. Everything about motion is still theory.
- `pca9685_trim_oscillator()` is still dead code: the oscillator has not been
  measured, and there is no way to apply a measurement without a reflash.

## Deferred

- Replace the Grove Vision string-scraping with a real JSON parse
- Second mechanism synchronized over the network
- Servo current sensing as a stall detector
