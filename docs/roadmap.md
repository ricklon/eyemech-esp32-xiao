# Roadmap

## M1 — Get the port compiling and running (current)

- [x] Repo restructured: MicroPython original preserved as reference
- [x] `pca9685`, `eye_servo`, `eye_motion`, `eye_vision`, `eye_web` scaffolded
- [x] Board abstraction so C6 and S3 both build
- [ ] **First build.** Expect include-path and IDF API drift — `i2c_master.h`
      needs IDF ≥ 5.2; if PlatformIO's toolchain is older, either pin a newer
      platform version or fall back to the legacy `driver/i2c.h`
- [ ] Bench test: PCA9685 answers on I²C at 0x40, `/OE` holds servos limp
- [ ] One servo on channel 0, unloaded, centres on command
- [ ] All six servos, calibrate limits from the control page, save to NVS
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

## Deferred

- Replace the Grove Vision string-scraping with a real JSON parse
- Second mechanism synchronized over the network
- Servo current sensing as a stall detector
