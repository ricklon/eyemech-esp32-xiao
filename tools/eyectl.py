#!/usr/bin/env python3
"""eyectl — poke the eyemech control API from the command line.

    python tools/eyectl.py --host 192.168.1.50 state
    python tools/eyectl.py --host eyemech.local look 0.4 -0.2
    python tools/eyectl.py --host eyemech.local mode calibrate
    python tools/eyectl.py --host eyemech.local jog pan 1450
    python tools/eyectl.py --host eyemech.local cal pan 950 1500 2050
    python tools/eyectl.py --host eyemech.local save

Standard library only — no pip install needed.
"""
import argparse
import json
import sys
import urllib.error
import urllib.request


def call(host, path, body=None):
    url = f"http://{host}{path}"
    data = json.dumps(body).encode() if body is not None else None
    req = urllib.request.Request(
        url, data=data, method="POST" if data is not None else "GET",
        headers={"Content-Type": "application/json"} if data is not None else {})
    try:
        with urllib.request.urlopen(req, timeout=5) as resp:
            return json.loads(resp.read() or b"{}")
    except urllib.error.HTTPError as e:
        sys.exit(f"HTTP {e.code}: {e.read().decode(errors='replace')}")
    except OSError as e:
        sys.exit(f"cannot reach {host}: {e}")


def main():
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--host", default="eyemech.local")
    sub = p.add_subparsers(dest="cmd", required=True)

    sub.add_parser("state")
    sub.add_parser("blink")
    sub.add_parser("save")

    m = sub.add_parser("mode"); m.add_argument("mode", choices=["idle", "manual", "calibrate"])
    lk = sub.add_parser("look"); lk.add_argument("x", type=float); lk.add_argument("y", type=float)
    lk.add_argument("--speed", type=float, default=0.3)
    ld = sub.add_parser("lids"); ld.add_argument("upper", type=float); ld.add_argument("lower", type=float)
    j = sub.add_parser("jog"); j.add_argument("axis"); j.add_argument("us", type=int)
    c = sub.add_parser("cal")
    c.add_argument("axis"); c.add_argument("min_us", type=int)
    c.add_argument("center_us", type=int); c.add_argument("max_us", type=int)
    c.add_argument("--inverted", action="store_true")

    a = p.parse_args()
    if a.cmd == "state":
        print(json.dumps(call(a.host, "/api/state"), indent=2))
    elif a.cmd == "blink":
        call(a.host, "/api/blink", {})
    elif a.cmd == "save":
        call(a.host, "/api/cal/save", {})
    elif a.cmd == "mode":
        call(a.host, "/api/mode", {"mode": a.mode})
    elif a.cmd == "look":
        call(a.host, "/api/look", {"x": a.x, "y": a.y, "speed": a.speed})
    elif a.cmd == "lids":
        call(a.host, "/api/lids", {"upper": a.upper, "lower": a.lower, "speed": 0.4})
    elif a.cmd == "jog":
        call(a.host, "/api/jog", {"axis": a.axis, "us": a.us})
    elif a.cmd == "cal":
        call(a.host, "/api/cal", {"axis": a.axis, "min_us": a.min_us,
                                  "center_us": a.center_us, "max_us": a.max_us,
                                  "inverted": a.inverted})


if __name__ == "__main__":
    main()
