#!/usr/bin/env python3
"""eyectl -- drive the eyemech HTTP API from the command line.

    python tools/eyectl.py --host 192.168.1.50 state
    python tools/eyectl.py mode calibration
    python tools/eyectl.py look 110 80
    python tools/eyectl.py servo TL 120        # calibration mode only
    python tools/eyectl.py limits TL 90 170
    python tools/eyectl.py cfg TL --trim-us 40
    python tools/eyectl.py trim 0.7
    python tools/eyectl.py save
    python tools/eyectl.py release

Standard library only -- no pip install needed.
"""
import argparse
import json
import sys
import urllib.error
import urllib.request


def call(host, path, body=None):
    url = "http://{}{}".format(host, path)
    data = json.dumps(body).encode() if body is not None else None
    req = urllib.request.Request(
        url, data=data, method="POST" if data is not None else "GET",
        headers={"Content-Type": "application/json"} if data is not None else {})
    try:
        with urllib.request.urlopen(req, timeout=5) as resp:
            return json.loads(resp.read() or b"{}")
    except urllib.error.HTTPError as e:
        sys.exit("HTTP {}: {}".format(e.code, e.read().decode(errors="replace")))
    except OSError as e:
        sys.exit("cannot reach {}: {}".format(host, e))


def print_state(s):
    print("{}  mode={}  wifi={} {}  vision={}  lid_trim={:.2f}".format(
        s.get("board", "?"), s.get("mode"), s.get("wifi_mode", "?"),
        s.get("wifi_ssid", "?"), s.get("vision"), s.get("lid_trim", 0)))
    print("{:<3} {:<4} {:>8} {:>7} {:>7} {:>8}".format(
        "ch", "name", "angle", "min", "max", "trim_us"))
    for sv in s.get("servos", []):
        angle = sv.get("angle")
        angle = "--" if angle is None else "{:.1f}".format(angle)
        print("{:<3} {:<4} {:>8} {:>7} {:>7} {:>8}".format(
            sv["channel"], sv["name"], angle, sv["min"], sv["max"], sv["trim_us"]))


def main():
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--host", default="eyemech.local")
    sub = p.add_subparsers(dest="cmd", required=True)

    sub.add_parser("state")
    sub.add_parser("blink")
    sub.add_parser("save")
    sub.add_parser("release")

    m = sub.add_parser("mode")
    m.add_argument("mode", choices=["tracking", "auto", "manual", "calibration"])

    lk = sub.add_parser("look")
    lk.add_argument("lr", type=float)
    lk.add_argument("ud", type=float)

    tr = sub.add_parser("trim")
    tr.add_argument("value", type=float, help="0.0 .. 1.0")

    sv = sub.add_parser("servo")
    sv.add_argument("servo")
    sv.add_argument("angle", type=float)

    li = sub.add_parser("limits")
    li.add_argument("servo")
    li.add_argument("min", type=float)
    li.add_argument("max", type=float)

    cf = sub.add_parser("cfg")
    cf.add_argument("servo")
    cf.add_argument("--min-us", type=int)
    cf.add_argument("--max-us", type=int)
    cf.add_argument("--trim-us", type=int)

    a = p.parse_args()

    if a.cmd == "state":
        print_state(call(a.host, "/api/state"))
    elif a.cmd in ("blink", "save", "release"):
        call(a.host, "/api/" + a.cmd, {})
    elif a.cmd == "mode":
        call(a.host, "/api/mode", {"mode": a.mode})
    elif a.cmd == "look":
        call(a.host, "/api/look", {"lr": a.lr, "ud": a.ud})
    elif a.cmd == "trim":
        call(a.host, "/api/lid_trim", {"value": a.value})
    elif a.cmd == "servo":
        call(a.host, "/api/servo", {"servo": a.servo, "angle": a.angle})
    elif a.cmd == "limits":
        call(a.host, "/api/limits", {"servo": a.servo, "min": a.min, "max": a.max})
    elif a.cmd == "cfg":
        body = {"servo": a.servo}
        for key, val in (("min_us", a.min_us), ("max_us", a.max_us), ("trim_us", a.trim_us)):
            if val is not None:
                body[key] = val
        if len(body) == 1:
            sys.exit("cfg needs at least one of --min-us / --max-us / --trim-us")
        call(a.host, "/api/cfg", body)


if __name__ == "__main__":
    main()
