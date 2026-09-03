# Roadmap

## M1 — Motion + web control (current)

- [x] Repo scaffold, PlatformIO + ESP-IDF build config
- [x] `eye_servo`: LEDC driver, calibration struct, NVS persistence
- [x] `eye_motion`: 50 Hz pose loop, easing, idle saccades, blink
- [x] `eye_web`: WiFi STA, REST API, embedded control page
- [ ] Confirm axis list and pin map against the real mechanism
- [ ] First build on Windows — resolve whatever the compiler objects to
- [ ] Bench test with one servo, unloaded, before anything is bolted in
- [ ] Calibrate all axes, save to NVS
- [ ] Tune easing and saccade timing until idle motion reads as lifelike

## M2 — Behavior

- [ ] Blink timing with natural variance rather than uniform random
- [ ] Coupled lid motion: lids follow tilt slightly, as real eyelids do
- [ ] Named expressions (neutral, alert, sleepy, squint) as pose presets
- [ ] Smooth pursuit vs. saccade — different easing profiles per movement type

## M3 — Integration

- [ ] Decide the xiaozhi relationship: does the eye firmware take commands from
      the voice agent, or run standalone and subscribe to agent state?
- [ ] If integrated: a small command protocol (WebSocket or MQTT) mapping agent
      states — listening, thinking, speaking — onto eye behavior
- [ ] OTA updates, so the mechanism doesn't need disassembly to reflash

## Deferred / maybe

- Gaze tracking from a camera (XIAO Sense variant has one)
- Second mechanism, synchronized over the network
- Servo current sensing as a stall detector
