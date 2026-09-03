"""
pca9685.py -- minimal PCA9685 driver for MicroPython.

Written for driving hobby servos: one shared frequency across all 16
channels, ON-time always 0 so only the OFF tick needs computing.

Clone boards frequently do NOT have a true 25 MHz oscillator -- 24 to 27 MHz
is common. If your servos land consistently off-centre, measure the actual
output period on a scope or logic analyser and pass the corrected value as
osc_hz. See trim_oscillator() at the bottom of this file.
"""

import time

_MODE1 = 0x00
_MODE2 = 0x01
_PRESCALE = 0xFE
_LED0_ON_L = 0x06
_ALL_LED_ON_L = 0xFA

_MODE1_RESTART = 0x80
_MODE1_AI = 0x20      # register auto-increment
_MODE1_SLEEP = 0x10
_MODE1_ALLCALL = 0x01
_MODE2_OUTDRV = 0x04  # totem-pole outputs (required for servos)


class PCA9685:
    def __init__(self, i2c, address=0x40, freq=50, osc_hz=25_000_000):
        self.i2c = i2c
        self.address = address
        self.osc_hz = osc_hz
        self.period_us = 1_000_000 / freq
        self._buf = bytearray(4)
        self.reset()
        self.set_freq(freq)

    def _w8(self, reg, val):
        self.i2c.writeto_mem(self.address, reg, bytes((val,)))

    def _r8(self, reg):
        return self.i2c.readfrom_mem(self.address, reg, 1)[0]

    def reset(self):
        self._w8(_MODE1, _MODE1_AI | _MODE1_ALLCALL)
        self._w8(_MODE2, _MODE2_OUTDRV)
        time.sleep_ms(5)

    def set_freq(self, freq):
        # Datasheet: prescale = round(osc / (4096 * freq)) - 1
        prescale = int(self.osc_hz / (4096 * freq) + 0.5) - 1
        prescale = max(3, min(255, prescale))

        # PRESCALE is only writable while the oscillator is asleep.
        old = self._r8(_MODE1)
        self._w8(_MODE1, (old & ~_MODE1_RESTART) | _MODE1_SLEEP)
        self._w8(_PRESCALE, prescale)
        self._w8(_MODE1, old)
        time.sleep_ms(5)
        self._w8(_MODE1, old | _MODE1_RESTART | _MODE1_AI)

        self.period_us = 1_000_000 / freq
        return prescale

    def set_pwm(self, channel, on, off):
        self._buf[0] = on & 0xFF
        self._buf[1] = (on >> 8) & 0x1F
        self._buf[2] = off & 0xFF
        self._buf[3] = (off >> 8) & 0x1F
        self.i2c.writeto_mem(self.address, _LED0_ON_L + 4 * channel, self._buf)

    def set_us(self, channel, us):
        """Set pulse width in microseconds. us <= 0 means output full off."""
        if us <= 0:
            self.set_pwm(channel, 0, 0x1000)  # bit 12 of OFF = full off
            return
        ticks = int(us * 4096 / self.period_us)
        if ticks > 4095:
            ticks = 4095
        self.set_pwm(channel, 0, ticks)

    def all_off(self):
        """Release every channel at once (servos go limp)."""
        self.i2c.writeto_mem(self.address, _ALL_LED_ON_L, bytes((0, 0, 0, 0x10)))

    def sleep(self):
        self._w8(_MODE1, self._r8(_MODE1) | _MODE1_SLEEP)

    def wake(self):
        self._w8(_MODE1, self._r8(_MODE1) & ~_MODE1_SLEEP)
        time.sleep_ms(1)


def trim_oscillator(measured_hz_at_50hz):
    """
    Helper for correcting a clone board's oscillator.

    Command 50 Hz, measure the ACTUAL frequency on any channel, then:

        real_osc = trim_oscillator(measured)
        pca = PCA9685(i2c, osc_hz=real_osc)

    e.g. if you asked for 50 Hz and measured 52.4 Hz, the oscillator is
    running about 4.8% fast and this returns roughly 26.2 MHz.
    """
    return int(25_000_000 * (measured_hz_at_50hz / 50.0))
