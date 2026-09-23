#!/usr/bin/env python3
"""eyemech_mcp -- a stdio MCP server that forwards to the board's POST /mcp.

    claude mcp add eyemech -- python3 /path/to/tools/eyemech_mcp.py
    claude mcp add eyemech -- python3 /path/to/tools/eyemech_mcp.py --host 192.168.9.1

Registered by URL, the board is probed once when a client session starts; if it
is unplugged then, the server is marked failed and its tools are gone for the
whole session. This process always starts, so the tools are always listed, and
a call made while the board is away comes back as a tool error saying so. When
the board returns, the next call reaches it -- the board's protocol is
stateless, so there is nothing to re-establish.

It is a byte pump, not a second implementation: every request goes to the board
unchanged, with the HTTP headers the Streamable HTTP transport requires derived
from the body. Only the answers that change with a reflash -- initialize,
server/discover, tools/list -- are cached, under ~/.cache/eyemech/. With a
cache they are answered at once and refreshed from the board in the background,
so a new tool shows up in the session after the one that first sees it. With no
cache and no board, tools/list fails: run once with the board up to seed it.

Standard library only -- no pip install needed. Logs go to stderr.
"""
import argparse
import base64
import json
import os
import socket
import sys
import threading
import urllib.error
import urllib.parse
import urllib.request

MCP_MODERN = "2026-07-28"
META_VERSION = "io.modelcontextprotocol/protocolVersion"
# Answers that only change when the firmware is reflashed.
CACHEABLE = ("initialize", "server/discover", "tools/list")


def log(msg):
    print("eyemech_mcp: " + msg, file=sys.stderr, flush=True)


def header_value(v):
    """Mcp-Name must be header-safe; anything else goes as =?base64?...?=."""
    if all(0x20 <= ord(c) < 0x7F for c in v) and v == v.strip():
        return v
    return "=?base64?" + base64.b64encode(v.encode()).decode() + "?="


class Board:
    def __init__(self, host, timeout, cache_path):
        self.host = host
        self.timeout = timeout
        self.cache_path = cache_path
        self.legacy_version = None   # negotiated by a legacy initialize
        self.lock = threading.Lock()
        self.cache = self._load_cache()
        # Resolving eyemech.local over mDNS can take seconds, and fails slowly
        # when the board is away. Once it has answered, talk to its address.
        # It is only that host's address: pointing --host somewhere else must
        # not send commands to whatever this address was last time.
        cached = self.cache.get("_addr")
        if not isinstance(cached, dict):
            cached = {}          # an older cache stored a bare address string
        self.addr = cached.get("addr") if cached.get("host") == host else None

    # ------------------------------------------------------------- cache

    def _load_cache(self):
        try:
            with open(self.cache_path) as f:
                return json.load(f)
        except (OSError, ValueError):
            return {}

    def _save_cache(self):
        try:
            os.makedirs(os.path.dirname(self.cache_path), exist_ok=True)
            tmp = self.cache_path + ".tmp"
            with open(tmp, "w") as f:
                json.dump(self.cache, f, indent=1)
            os.replace(tmp, self.cache_path)
        except OSError as e:
            log("cannot write cache {}: {}".format(self.cache_path, e))

    @staticmethod
    def cache_key(msg):
        method = msg.get("method")
        params = msg.get("params") or {}
        modern = META_VERSION in (params.get("_meta") or {})
        key = method + (" modern" if modern else " legacy")
        if method == "initialize":
            key += " " + str(params.get("protocolVersion"))
        return key

    # --------------------------------------------------------------- HTTP

    def headers_for(self, msg):
        h = {"Content-Type": "application/json",
             "Accept": "application/json, text/event-stream"}
        params = msg.get("params") or {}
        meta = params.get("_meta") or {}
        if META_VERSION in meta:
            h["MCP-Protocol-Version"] = str(meta[META_VERSION])
            h["Mcp-Method"] = header_value(msg.get("method", ""))
            if msg.get("method") == "tools/call" and isinstance(params.get("name"), str):
                h["Mcp-Name"] = header_value(params["name"])
        elif self.legacy_version and msg.get("method") != "initialize":
            h["MCP-Protocol-Version"] = self.legacy_version
        return h

    def _post_to(self, target, data, headers):
        url = "http://{}/mcp".format(target)
        req = urllib.request.Request(url, data=data, method="POST", headers=headers)
        if target != self.host:
            req.add_header("Host", self.host)
        try:
            with urllib.request.urlopen(req, timeout=self.timeout) as resp:
                return resp.status, resp.read()
        except urllib.error.HTTPError as e:
            # 400/404 still carry a JSON-RPC error body meant for the client.
            return e.code, e.read()

    def post(self, msg):
        """Returns (status, body bytes). Raises OSError if the board is away."""
        data = json.dumps(msg).encode()
        headers = self.headers_for(msg)
        if self.addr:
            try:
                return self._post_to(self.addr, data, headers)
            except OSError:
                self.addr = None   # it may have a new lease; resolve again
        status, body = self._post_to(self.host, data, headers)
        self._remember_addr()
        return status, body

    def _remember_addr(self):
        name, sep, port = self.host.partition(":")
        try:
            addr = socket.gethostbyname(name) + sep + port
        except OSError:
            return
        if addr != self.host and addr != self.addr:
            with self.lock:
                self.addr = addr
                self.cache["_addr"] = {"host": self.host, "addr": addr}
                self._save_cache()

    # ----------------------------------------------------------- requests

    def forward(self, msg):
        """Forwards one request and returns the reply to write, or None."""
        status, body = self.post(msg)
        if not body:
            return None
        reply = json.loads(body)
        result = reply.get("result")
        if isinstance(result, dict):
            if msg.get("method") == "initialize" and isinstance(result.get("protocolVersion"), str):
                self.legacy_version = result["protocolVersion"]
            if msg.get("method") in CACHEABLE:
                with self.lock:
                    self.cache[self.cache_key(msg)] = result
                    self._save_cache()
        return reply

    def cached(self, msg):
        if msg.get("method") not in CACHEABLE:
            return None
        result = self.cache.get(self.cache_key(msg))
        if result is None:
            return None
        if msg["method"] == "initialize":
            self.legacy_version = result.get("protocolVersion")
        return {"jsonrpc": "2.0", "id": msg["id"], "result": result}

    def unreachable(self, msg, err):
        """The reply to a request the board could not be asked."""
        # On a timeout the board may have acted before the reply was lost, so
        # this must not claim nothing moved.
        text = ("The eyemech board at {} did not answer ({}). It is probably "
                "unplugged, rebooting, or off the network. Ask a person to check "
                "it, then call get_state before trying again.").format(self.host, err)
        method = msg.get("method")
        params = msg.get("params") or {}
        if method == "tools/call":
            result = {"content": [{"type": "text", "text": text}], "isError": True}
            if META_VERSION in (params.get("_meta") or {}):
                result["resultType"] = "complete"
            return {"jsonrpc": "2.0", "id": msg["id"], "result": result}
        if method == "ping":
            return {"jsonrpc": "2.0", "id": msg["id"], "result": {}}
        return {"jsonrpc": "2.0", "id": msg["id"],
                "error": {"code": -32603, "message": text}}

    def handle(self, msg):
        if "id" not in msg:
            # A notification: the board only acknowledges these, so losing one
            # while it is away costs nothing.
            try:
                self.post(msg)
            except OSError:
                pass
            return None
        hit = self.cached(msg)
        if hit is not None:
            threading.Thread(target=self._refresh, args=(msg,), daemon=True).start()
            return hit
        try:
            return self.forward(msg)
        except OSError as e:
            log("{} failed: {}".format(msg.get("method"), e))
            return self.unreachable(msg, reason(e))
        except ValueError as e:
            self.addr = None   # something else may have taken the address
            return self.unreachable(msg, "bad reply: {}".format(e))

    def _refresh(self, msg):
        try:
            self.forward(msg)
        except (OSError, ValueError):
            pass


def reason(e):
    if isinstance(e, urllib.error.URLError) and e.reason is not None:
        e = e.reason
    if isinstance(e, socket.timeout) or isinstance(e, TimeoutError):
        return "timed out"
    return str(e) or type(e).__name__


def default_cache(host):
    base = os.environ.get("XDG_CACHE_HOME") or os.path.expanduser("~/.cache")
    safe = urllib.parse.quote(host, safe="")
    return os.path.join(base, "eyemech", "mcp-{}.json".format(safe))


def main():
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--host", default=os.environ.get("EYEMECH_HOST", "eyemech.local"),
                   help="board host[:port] (default eyemech.local, or $EYEMECH_HOST)")
    p.add_argument("--timeout", type=float, default=4.0,
                   help="seconds to wait for the board per request (default 4)")
    p.add_argument("--cache", help="cache file (default ~/.cache/eyemech/mcp-<host>.json)")
    args = p.parse_args()

    board = Board(args.host, args.timeout, args.cache or default_cache(args.host))
    out_lock = threading.Lock()

    def serve(msg):
        reply = board.handle(msg)
        if reply is not None:
            with out_lock:
                sys.stdout.write(json.dumps(reply, separators=(",", ":")) + "\n")
                sys.stdout.flush()

    # One thread per request, so a slow or unreachable board holding one call
    # does not stall the rest.
    workers = []
    for line in sys.stdin:
        line = line.strip()
        if not line:
            continue
        try:
            msg = json.loads(line)
        except ValueError:
            parse_error = {"jsonrpc": "2.0", "id": None,
                           "error": {"code": -32700, "message": "Parse error"}}
            with out_lock:
                sys.stdout.write(json.dumps(parse_error) + "\n")
                sys.stdout.flush()
            continue
        if not isinstance(msg, dict) or not isinstance(msg.get("method"), str):
            continue   # a response to something we never send, or a batch
        t = threading.Thread(target=serve, args=(msg,), daemon=True)
        t.start()
        workers = [w for w in workers if w.is_alive()] + [t]
    # stdin closed: let the replies already in flight go out before exiting.
    for w in workers:
        w.join()


if __name__ == "__main__":
    main()
