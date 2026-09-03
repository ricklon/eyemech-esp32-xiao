"""
Will Cogley's Eye Mechanism -- ported to Seeed XIAO ESP32C6 + PCA9685.

Original: Raspberry Pi Pico, micropython-servo, direct GPIO PWM.
This port: all six servos on a PCA9685 over I2C, freeing the ESP32-C6's
six LEDC channels entirely and giving hardware-timed pulses.

Requires pca9685.py and servo.py alongside this file.
"""

import time
import random
from machine import Pin, I2C, ADC, UART

from pca9685 import PCA9685
from servo import Servo

# ---------------------------------------------------------------------------
# Configuration -- the values you will actually need to tune
# ---------------------------------------------------------------------------

I2C_FREQ = 400_000        # 100 kHz caps the loop near 330 Hz. Use 400 kHz.
PCA_ADDRESS = 0x40
PCA_OSC_HZ = 25_000_000   # see pca9685.trim_oscillator() if servos sit off-centre

# ADC endpoints. These MUST be measured on your hardware -- ESP32 ADC is
# nonlinear and nothing from the Pico version carries over. Run
# calibrate_pots() from the REPL and record the extremes.
POT_MIN = 1500
POT_MAX = 64000
TRIM_MIN = 1500
TRIM_MAX = 64000

# Blink timing is now wall-clock, not loop-iteration count. The original
# random.randrange(20000) was tied to the Pico's loop rate and does not
# transfer to a different core at a different clock.
BLINK_GAP_MIN_MS = 2000
BLINK_GAP_MAX_MS = 7000
BLINK_CLOSED_MS = 70
BLINK_OPENING_MS = 70

# ---------------------------------------------------------------------------
# Hardware
# ---------------------------------------------------------------------------

# XIAO ESP32C6 user LED is on GPIO15 and is ACTIVE LOW -- inverted from
# the Pico's GPIO25. value(0) lights it.
led = Pin(15, Pin.OUT)
led.value(1)  # off

# PCA9685 /OE is active low. Hold it HIGH (outputs disabled) until the
# servos have a sane commanded position, so nothing slams at reset.
# Fit a 10k pull-up from /OE to 3V3 so the line is held disabled even
# while this GPIO floats during the bootloader window.
oe = Pin(18, Pin.OUT, value=1)          # D10

enable_sw = Pin(21, Pin.IN, Pin.PULL_UP)   # D3
mode_sw = Pin(19, Pin.IN, Pin.PULL_UP)     # D8
blink_btn = Pin(20, Pin.IN, Pin.PULL_UP)   # D9

UD = ADC(Pin(0))     # D0 / A0
trim = ADC(Pin(1))   # D1 / A1
LR = ADC(Pin(2))     # D2 / A2
for _adc in (UD, trim, LR):
    _adc.atten(ADC.ATTN_11DB)   # full ~0-3.1 V range; default caps near 1 V

i2c = I2C(0, sda=Pin(22), scl=Pin(23), freq=I2C_FREQ)   # D4 / D5
pca = PCA9685(i2c, address=PCA_ADDRESS, freq=50, osc_hz=PCA_OSC_HZ)

servos = {
    "LR": Servo(pca, 0),
    "UD": Servo(pca, 1),
    "TL": Servo(pca, 2),
    "BL": Servo(pca, 3),
    "TR": Servo(pca, 4),
    "BR": Servo(pca, 5),
}

# Min, Max -- unchanged from the original. BL and TR run backwards because
# those two lid servos are mounted mirrored relative to their partners.
servo_limits = {
    "LR": (40, 140),
    "UD": (40, 140),
    "TL": (90, 170),
    "BL": (90, 10),
    "TR": (90, 10),
    "BR": (90, 160),
}

# ---------------------------------------------------------------------------
# State
# ---------------------------------------------------------------------------

tl_target = 90
tr_target = 90
bl_target = 90
br_target = 90

x_target = 90
y_target = 90

# ---------------------------------------------------------------------------
# Motion primitives -- logic identical to the original
# ---------------------------------------------------------------------------


def calibrate():
    """All servos to 90 degrees, for fitting horns and linkages."""
    for servo in servos.values():
        servo.write(90)


def neutral():
    for servo in servos.values():
        servo.write(90)
    for name in ("TL", "BL", "TR", "BR"):
        servos[name].write(servo_limits[name][1])


def blink():
    servos["TL"].write(servo_limits["TL"][0])
    servos["TR"].write(servo_limits["TR"][0])
    servos["BL"].write(servo_limits["BL"][0])
    servos["BR"].write(servo_limits["BR"][0])


def open_lid():
    servos["TL"].write(tl_target)
    servos["TR"].write(tr_target)
    servos["BL"].write(bl_target)
    servos["BR"].write(br_target)


def control_ud_and_lids(ud_angle):
    """Move UD and have the four lids follow its position."""
    global tl_target, tr_target, bl_target, br_target

    ud_min, ud_max = servo_limits["UD"]
    tl_min, tl_max = servo_limits["TL"]
    tr_min, tr_max = servo_limits["TR"]
    bl_min, bl_max = servo_limits["BL"]
    br_min, br_max = servo_limits["BR"]

    ud_progress = (ud_angle - ud_min) / (ud_max - ud_min)

    tl_target = tl_max - ((tl_max - tl_min) * (0.8 * (1 - ud_progress)))
    tr_target = tr_max + ((tr_min - tr_max) * (0.8 * (1 - ud_progress)))
    bl_target = bl_max + ((bl_min - bl_max) * (0.4 * ud_progress))
    br_target = br_max - ((br_max - br_min) * (0.4 * ud_progress))

    servos["UD"].write(ud_angle)
    servos["TL"].write(tl_target)
    servos["TR"].write(tr_target)
    servos["BL"].write(bl_target)
    servos["BR"].write(br_target)


def scale_potentiometer(pot_value, servo, reverse=False):
    min_limit, max_limit = servo_limits[servo]
    scaled = min_limit + (pot_value - POT_MIN) * (max_limit - min_limit) / (
        POT_MAX - POT_MIN)
    if reverse:
        scaled = max_limit - (scaled - min_limit)
    return scaled


def map_value(value, in_min, in_max, out_min, out_max):
    return (value - in_min) * (out_max - out_min) / (in_max - in_min) + out_min


def update_eyelid_limits(trim_value):
    """Live trim of how wide the eyes open. Controller mode only."""
    TL_max_range = (130, 170)
    BR_max_range = (130, 170)
    BL_max_range = (50, 10)
    TR_max_range = (50, 10)

    progress = (trim_value - TRIM_MIN) / (TRIM_MAX - TRIM_MIN)
    progress = max(0.0, min(1.0, progress))

    servo_limits["TL"] = (90, TL_max_range[0] + (TL_max_range[1] - TL_max_range[0]) * progress)
    servo_limits["BR"] = (90, BR_max_range[0] + (BR_max_range[1] - BR_max_range[0]) * progress)
    servo_limits["BL"] = (90, BL_max_range[0] + (BL_max_range[1] - BL_max_range[0]) * progress)
    servo_limits["TR"] = (90, TR_max_range[0] + (TR_max_range[1] - TR_max_range[0]) * progress)


def calibrate_pots():
    """
    REPL helper. Sweep each pot end to end and record the extremes, then
    put them into POT_MIN / POT_MAX / TRIM_MIN / TRIM_MAX above.
    Ctrl-C to stop.
    """
    try:
        while True:
            print("UD {:5d}   trim {:5d}   LR {:5d}".format(
                UD.read_u16(), trim.read_u16(), LR.read_u16()))
            time.sleep_ms(200)
    except KeyboardInterrupt:
        pass


# ---------------------------------------------------------------------------
# Grove Vision AI comms
# ---------------------------------------------------------------------------


class Comms:
    def __init__(self):
        # UART1 on D6/D7. rxbuf matters: at 921600 the default buffer
        # overruns mid-packet and silently corrupts the box parsing.
        self.grove = UART(1, baudrate=921600, tx=Pin(16), rx=Pin(17),
                          rxbuf=2048, timeout=0)

        self.INVOKE_CMD = b"AT+INVOKE=1,0,1\r"
        self.pixel_centre = 112
        self.deadzone = 20
        self.x_adj_factor = 10
        self.y_adj_factor = 10
        self.staticflag = False

        self.cbuf = b""
        self.readflag = True
        self.last_boxes = None

        self.grove.write(self.INVOKE_CMD)
        self.grove_vision_module = False

    def map_value(self, value, in_min, in_max, out_min, out_max):
        return (value - in_min) * (out_max - out_min) / (in_max - in_min) + out_min

    def grove_read(self):
        if self.readflag:
            while self.grove.any():
                self.grove.read()
                self.grove_vision_module = True
            self.grove.write(self.INVOKE_CMD)
            self.cbuf = b""
            self.readflag = False
            return None

        if self.grove.any():
            chunk = self.grove.read()
            if chunk:
                self.cbuf += chunk

            # Guard against a runaway buffer if the module never sends
            # the resolution key -- the Pico version could grow forever.
            if len(self.cbuf) > 4096:
                self.cbuf = b""
                self.readflag = True
                return None

            if b'"resolution"' in self.cbuf:
                key = b'"boxes":'
                i = self.cbuf.find(key)
                if i != -1:
                    boxes_part = self.cbuf[i + len(key):]
                    end_idx = boxes_part.find(b']')
                    if end_idx != -1:
                        boxes_part = boxes_part[:end_idx + 1].strip()

                        if boxes_part != b'[]' and boxes_part != self.last_boxes:
                            self.staticflag = False
                            self.last_boxes = boxes_part
                            boxes_str = boxes_part.decode('utf-8').strip('[]')
                            try:
                                numbers = [int(n) for n in boxes_str.split(',')]
                                x_offset = numbers[0] - self.pixel_centre
                                y_offset = numbers[1] - self.pixel_centre
                                self.cbuf = b""
                                self.readflag = True
                                return x_offset, y_offset
                            except (ValueError, IndexError):
                                self.cbuf = b""
                                self.readflag = True
                        else:
                            self.cbuf = b""
                            self.readflag = True
                            self.staticflag = True
                            return None
        return None


# ---------------------------------------------------------------------------
# Startup
# ---------------------------------------------------------------------------

led.value(0)  # on -- probing

neutral()
oe.value(0)   # outputs live, now that positions are commanded

comms = Comms()
time.sleep_ms(100)
comms.grove_read()
work_mode = "tracking" if comms.grove_vision_module else "auto"

led.value(1)  # off

# Blink state machine
blink_phase = None   # None | "closed" | "opening"
blink_until = 0
next_blink_at = time.ticks_add(
    time.ticks_ms(), random.randint(BLINK_GAP_MIN_MS, BLINK_GAP_MAX_MS))

# ---------------------------------------------------------------------------
# Main loop
# ---------------------------------------------------------------------------

while True:
    now = time.ticks_ms()
    work_mode_copy = work_mode

    mode_state = not mode_sw.value()
    enable_state = not enable_sw.value()

    if mode_state:
        work_mode = "calibration"
    else:
        if enable_state:
            work_mode = "controller"
        elif work_mode == "controller":
            work_mode = "tracking"

    if work_mode == "tracking":
        if blink_phase is None and time.ticks_diff(now, next_blink_at) >= 0:
            blink_phase = "closed"
            blink_until = time.ticks_add(now, BLINK_CLOSED_MS)
            blink()

        if blink_phase == "closed" and time.ticks_diff(now, blink_until) >= 0:
            blink_phase = "opening"
            blink_until = time.ticks_add(now, BLINK_OPENING_MS)
            open_lid()
        elif blink_phase == "opening" and time.ticks_diff(now, blink_until) >= 0:
            blink_phase = None
            next_blink_at = time.ticks_add(
                now, random.randint(BLINK_GAP_MIN_MS, BLINK_GAP_MAX_MS))

        offset = comms.grove_read()
        if offset:
            x_offset, y_offset = offset

            if abs(x_offset) > comms.deadzone and not comms.staticflag:
                x_adj = comms.map_value(x_offset, -110, 110,
                                        comms.x_adj_factor, -comms.x_adj_factor)
                x_target = max(servo_limits["LR"][0],
                               min(x_target + x_adj, servo_limits["LR"][1]))
                servos["LR"].write(x_target)

            if abs(y_offset) > comms.deadzone and not comms.staticflag:
                y_adj = comms.map_value(y_offset, -110, 110,
                                        comms.y_adj_factor, -comms.y_adj_factor)
                y_target = max(servo_limits["UD"][0],
                               min(y_target + y_adj, servo_limits["UD"][1]))

            if blink_phase is None:
                control_ud_and_lids(y_target)

    elif work_mode == "calibration":
        calibrate()
        comms.grove_read()
        work_mode = "initialisation"

    elif work_mode == "initialisation":
        blink()
        work_mode = "tracking" if comms.grove_vision_module else "auto"

    elif work_mode == "controller":
        UD_value = UD.read_u16()
        trim_value = trim.read_u16()
        LR_value = LR.read_u16()

        update_eyelid_limits(trim_value)

        if not blink_btn.value():
            blink()
        else:
            servos["LR"].write(scale_potentiometer(LR_value, "LR", reverse=True))
            control_ud_and_lids(scale_potentiometer(UD_value, "UD"))
        time.sleep_ms(10)

    elif work_mode == "auto":
        command = random.randint(0, 2)
        if command == 0:
            blink()
            time.sleep_ms(100)
            open_lid()
        elif command == 1:
            blink()
            time.sleep_ms(100)
            control_ud_and_lids(random.randint(*servo_limits["UD"]))
            servos["LR"].write(random.randint(*servo_limits["LR"]))
            time.sleep_ms(random.randint(300, 1000))
        else:
            control_ud_and_lids(random.randint(*servo_limits["UD"]))
            servos["LR"].write(random.randint(*servo_limits["LR"]))
            time.sleep_ms(random.randint(200, 400))

    if work_mode != work_mode_copy:
        neutral()
        blink_phase = None
