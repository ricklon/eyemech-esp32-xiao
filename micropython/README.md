# The MicroPython original

These three files are the working implementation: XIAO ESP32-C6, MicroPython,
all six servos on a PCA9685. They are kept as the **reference** for the ESP-IDF
port in the parent directory.

If the C port behaves differently from this code, the C port is wrong — unless
the difference is recorded in `../docs/decisions.md`. Don't edit these files to
make the port look correct.

| File | Ported to |
|---|---|
| `pca9685.py` | `components/pca9685/` |
| `servo.py` | `components/eye_servo/` |
| `main.py` | `components/eye_motion/`, `components/eye_vision/` |

Deploy:

```
mpremote connect COM5 cp pca9685.py servo.py main.py :
```

Notes on what changed in the Pico → ESP32-C6 port that produced these files are
at the bottom of `../docs/WIRING.md`.
