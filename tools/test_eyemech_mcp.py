#!/usr/bin/env python3
"""Host tests for eyemech_mcp.py against a fake board. No hardware needed.

    python3 tools/test_eyemech_mcp.py
"""
import http.server
import json
import os
import socket
import subprocess
import sys
import tempfile
import threading
import time
import unittest

PROXY = os.path.join(os.path.dirname(os.path.abspath(__file__)), "eyemech_mcp.py")
META_VERSION = "io.modelcontextprotocol/protocolVersion"
TOOLS = {"tools": [{"name": "get_state", "inputSchema": {"type": "object"}}]}


class FakeBoard(http.server.BaseHTTPRequestHandler):
    """Answers the way eye_mcp.c does, and records the headers it was sent."""
    seen = []

    def do_POST(self):
        msg = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
        FakeBoard.seen.append((msg, self.headers))
        if "id" not in msg:
            self.send_response(202)
            self.end_headers()
            return
        params = msg.get("params") or {}
        modern = META_VERSION in (params.get("_meta") or {})
        if modern and (self.headers.get("MCP-Protocol-Version") != "2026-07-28"
                       or self.headers.get("Mcp-Method") != msg["method"]):
            return self.reply(400, {"error": {"code": -32020, "message": "Header mismatch"}}, msg)
        if msg["method"] == "initialize":
            result = {"protocolVersion": params.get("protocolVersion"), "capabilities": {"tools": {}}}
        elif msg["method"] == "tools/list":
            result = dict(TOOLS)
        elif msg["method"] == "tools/call":
            result = {"content": [{"type": "text", "text": "state"}]}
        else:
            return self.reply(404, {"error": {"code": -32601, "message": "Method not found"}}, msg)
        self.reply(200, {"result": result}, msg)

    def reply(self, status, body, msg):
        data = json.dumps(dict(body, jsonrpc="2.0", id=msg["id"])).encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def log_message(self, *args):
        pass


def free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def modern(method, id_, **params):
    params["_meta"] = {META_VERSION: "2026-07-28"}
    return {"jsonrpc": "2.0", "id": id_, "method": method, "params": params}


class ProxyTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.cache = os.path.join(self.tmp.name, "cache.json")
        self.port = free_port()
        self.server = None
        FakeBoard.seen = []

    def tearDown(self):
        self.board_down()
        self.tmp.cleanup()

    def board_up(self):
        self.server = http.server.ThreadingHTTPServer(("127.0.0.1", self.port), FakeBoard)
        threading.Thread(target=self.server.serve_forever, daemon=True).start()

    def board_down(self):
        if self.server:
            self.server.shutdown()
            self.server.server_close()
            self.server = None

    def run_proxy(self, *msgs):
        """Sends msgs one at a time, waiting for each reply, and returns them by id."""
        proc = subprocess.Popen(
            [sys.executable, PROXY, "--host", "127.0.0.1:{}".format(self.port),
             "--timeout", "1", "--cache", self.cache],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)
        replies = {}
        for m in msgs:
            proc.stdin.write(json.dumps(m) + "\n")
            proc.stdin.flush()
            if "id" in m:
                r = json.loads(proc.stdout.readline())
                replies[r["id"]] = r
        proc.stdin.close()
        proc.wait(timeout=10)
        proc.stdout.close()
        return replies

    def test_forwards_modern_with_headers_and_caches_tools(self):
        self.board_up()
        r = self.run_proxy(modern("tools/list", 1), modern("tools/call", 2, name="get_state"))
        self.assertEqual(r[1]["result"]["tools"], TOOLS["tools"])
        self.assertEqual(r[2]["result"]["content"][0]["text"], "state")
        _, h = FakeBoard.seen[-1]
        self.assertEqual(h["Mcp-Name"], "get_state")
        with open(self.cache) as f:
            self.assertIn("tools/list modern", json.load(f))

    def test_board_away_with_cache(self):
        self.board_up()
        self.run_proxy(modern("tools/list", 1))
        self.board_down()
        t0 = time.monotonic()
        r = self.run_proxy(modern("tools/list", 1), modern("tools/call", 2, name="get_state"))
        self.assertEqual(r[1]["result"]["tools"], TOOLS["tools"])
        self.assertTrue(r[2]["result"]["isError"])
        self.assertIn("did not answer", r[2]["result"]["content"][0]["text"])
        self.assertEqual(r[2]["result"]["resultType"], "complete")
        self.assertLess(time.monotonic() - t0, 8)

    def test_board_away_without_cache(self):
        r = self.run_proxy(modern("tools/list", 1))
        self.assertIn("did not answer", r[1]["error"]["message"])

    def test_board_returns_mid_session(self):
        proc_msgs = [modern("tools/call", 1, name="get_state")]
        r = self.run_proxy(*proc_msgs)
        self.assertTrue(r[1]["result"]["isError"])
        self.board_up()
        r = self.run_proxy(*proc_msgs)
        self.assertNotIn("isError", r[1]["result"])

    def test_legacy_carries_negotiated_version(self):
        self.board_up()
        init = {"jsonrpc": "2.0", "id": 1, "method": "initialize",
                "params": {"protocolVersion": "2025-06-18", "capabilities": {}}}
        note = {"jsonrpc": "2.0", "method": "notifications/initialized"}
        call = {"jsonrpc": "2.0", "id": 2, "method": "tools/call", "params": {"name": "get_state"}}
        r = self.run_proxy(init, note, call)
        self.assertEqual(r[2]["result"]["content"][0]["text"], "state")
        _, h = FakeBoard.seen[-1]
        self.assertEqual(h["MCP-Protocol-Version"], "2025-06-18")

    def test_remembered_address_is_not_used_for_another_host(self):
        """A cache naming another host's address must not be dialled: that would
        send commands to whatever now holds it."""
        self.board_up()
        self.run_proxy(modern("tools/list", 1))
        with open(self.cache) as f:
            cache = json.load(f)
        cache["_addr"] = {"host": "eyemech.local", "addr": "127.0.0.1:{}".format(self.port)}
        with open(self.cache, "w") as f:
            json.dump(cache, f)
        before = len(FakeBoard.seen)
        proc = subprocess.Popen(
            [sys.executable, PROXY, "--host", "192.0.2.1", "--timeout", "1", "--cache", self.cache],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)
        out, _ = proc.communicate(json.dumps(
            modern("tools/call", 1, name="get_state")) + "\n", timeout=30)
        self.assertTrue(json.loads(out)["result"]["isError"])
        self.assertEqual(len(FakeBoard.seen), before)   # the fake board was never dialled

    def test_board_error_bodies_pass_through(self):
        self.board_up()
        r = self.run_proxy(modern("nope", 1))
        self.assertEqual(r[1]["error"]["code"], -32601)


if __name__ == "__main__":
    unittest.main()
