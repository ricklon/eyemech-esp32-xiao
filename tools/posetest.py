#!/usr/bin/env python3
"""posetest -- drive follow mode over /ws/pose without a camera.

Sends a slow, small figure around the centre with the lids open, so the stream
path can be checked on the mechanism before a tracker is attached. Stop it with
Ctrl-C; the board eases back to neutral a second after poses stop.

    python tools/posetest.py --host eyemech.local --seconds 10
    python tools/posetest.py --amplitude 0.15 --blink-every 3

The mechanism must be engaged and in auto, manual or tracking first. Standard
library only: this is a minimal WebSocket client, not a general one.
"""
import argparse
import base64
import json
import math
import os
import socket
import struct
import sys
import time


def ws_connect(host, port, path):
    s = socket.create_connection((host, port), timeout=5)
    key = base64.b64encode(os.urandom(16)).decode()
    s.sendall((f"GET {path} HTTP/1.1\r\nHost: {host}\r\nUpgrade: websocket\r\n"
               f"Connection: Upgrade\r\nSec-WebSocket-Key: {key}\r\n"
               f"Sec-WebSocket-Version: 13\r\n\r\n").encode())
    head = b""
    while b"\r\n\r\n" not in head:
        chunk = s.recv(1024)
        if not chunk:
            sys.exit("connection closed during the WebSocket handshake")
        head += chunk
    if not head.startswith(b"HTTP/1.1 101"):
        sys.exit("WebSocket upgrade refused: " + head.split(b"\r\n")[0].decode(errors="replace"))
    s.setblocking(False)
    return s


def ws_send_text(s, text):
    data = text.encode()
    mask = os.urandom(4)
    header = bytes([0x81])
    n = len(data)
    if n < 126:
        header += bytes([0x80 | n])
    else:
        header += bytes([0x80 | 126]) + struct.pack(">H", n)
    s.setblocking(True)
    s.sendall(header + mask + bytes(b ^ mask[i % 4] for i, b in enumerate(data)))
    s.setblocking(False)


def ws_drain(s):
    """Print any {"error":...} frames the board sent back; poses get no reply."""
    try:
        buf = s.recv(4096)
    except BlockingIOError:
        return
    while len(buf) >= 2:
        n = buf[1] & 0x7F
        start = 2
        if n == 126:
            n = struct.unpack(">H", buf[2:4])[0]
            start = 4
        payload = buf[start:start + n]
        if buf[0] & 0x0F == 1:
            print("board:", payload.decode(errors="replace"))
        buf = buf[start + n:]


def main():
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("--host", default="eyemech.local")
    p.add_argument("--port", type=int, default=80)
    p.add_argument("--rate", type=float, default=30.0, help="poses per second")
    p.add_argument("--seconds", type=float, default=10.0)
    p.add_argument("--amplitude", type=float, default=0.1,
                   help="how far from centre the gaze wanders, 0..0.5")
    p.add_argument("--period", type=float, default=4.0, help="seconds per figure")
    p.add_argument("--blink-every", type=float, default=0.0,
                   help="seconds between blinks sent as lid values; 0 for none")
    p.add_argument("--paired", action="store_true",
                   help="send lid_l/lid_r instead of the four lids")
    a = p.parse_args()
    amp = max(0.0, min(0.5, a.amplitude))

    s = ws_connect(a.host, a.port, "/ws/pose")
    print(f"streaming to ws://{a.host}/ws/pose at {a.rate:g} Hz for {a.seconds:g} s")
    t0 = time.monotonic()
    try:
        while (t := time.monotonic() - t0) < a.seconds:
            phase = 2 * math.pi * t / a.period
            upper = 1.0
            if a.blink_every > 0 and (t % a.blink_every) < 0.15:
                upper = 0.0
            pose = {"lr": 0.5 + amp * math.sin(phase), "ud": 0.5 + amp * math.sin(2 * phase) / 2}
            if a.paired:
                pose.update(lid_l=upper, lid_r=upper)
            else:
                # eye-tracking's controller moves the lower lid less until nearly shut.
                lower = math.sqrt(upper)
                pose.update(lid_tl=upper, lid_bl=lower, lid_tr=upper, lid_br=lower)
            ws_send_text(s, json.dumps(pose))
            ws_drain(s)
            time.sleep(1.0 / a.rate)
    except KeyboardInterrupt:
        pass
    ws_send_text(s, json.dumps({"stop": True}))
    time.sleep(0.2)
    ws_drain(s)
    s.close()
    print("stopped; the board eases back to neutral")


if __name__ == "__main__":
    main()
