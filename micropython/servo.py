"""
servo.py -- Servo bound to a PCA9685 channel.

Keeps the same .write(angle) surface the original eye code used, so
control_ud_and_lids(), blink(), open_lid() and friends are untouched.
Only the constructor changes: Servo(driver, channel) instead of
Servo(pin_id=N).

Accepts float angles -- control_ud_and_lids() and update_eyelid_limits()
both produce them -- and clamps rather than raising.
"""


class Servo:
    def __init__(self, driver, channel, min_us=500, max_us=2500,
                 min_angle=0, max_angle=180, trim_us=0):
        self._d = driver
        self._ch = channel
        self._min_us = min_us
        self._us_span = max_us - min_us
        self._min_angle = min_angle
        self._max_angle = max_angle
        self._angle_span = max_angle - min_angle
        self._trim_us = trim_us   # per-servo mechanical centring offset
        self._last = None

    def write(self, angle):
        if angle < self._min_angle:
            angle = self._min_angle
        elif angle > self._max_angle:
            angle = self._max_angle

        # I2C is the bottleneck now -- the main loop rewrites identical
        # lid targets thousands of times a second. Skipping no-op writes
        # is worth roughly an order of magnitude in loop rate.
        if angle == self._last:
            return
        self._last = angle

        us = self._min_us + self._us_span * (
            (angle - self._min_angle) / self._angle_span) + self._trim_us
        self._d.set_us(self._ch, us)

    def read(self):
        """Last commanded angle, or None if never written."""
        return self._last

    def release(self):
        """Stop driving this servo. It goes limp."""
        self._d.set_us(self._ch, 0)
        self._last = None
