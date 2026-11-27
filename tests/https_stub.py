#!/usr/bin/env python3
"""An HTTPS stand-in for Qdrant's points/scroll and points/query, for the connector's tests only.

usage: https_stub.py --cert C --key K --port-file F --log L --api-key KEY --points P

It applies the filter shapes the connector sends (must, should, match value, match any) with Qdrant's
semantics (missing or null never matches, types must agree, an array matches when an element does),
appends "POST <path>" to the log for every request, and answers 401 without the right api-key header.
Collections named redirect, slow, big, fail503 and flaky misbehave on purpose.
"""
import argparse
import json
import os
import socketserver
import ssl
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

LOCK = threading.Lock()
FLAKY = [0]


def lookup(payload, path):
    node = payload
    for segment in path.split("."):
        if not isinstance(node, dict) or segment not in node:
            return None, False
        node = node[segment]
    return node, True


def same(a, b):
    return type(a) is type(b) and a == b


def matches(value, wanted):
    if isinstance(value, list):
        return any(same(v, wanted) for v in value)
    return same(value, wanted)


def condition(payload, cond):
    if "key" in cond:
        value, found = lookup(payload, cond["key"])
        if not found or value is None:
            return False
        match = cond["match"]
        if "value" in match:
            return matches(value, match["value"])
        return any(matches(value, w) for w in match["any"])
    return evaluate(payload, cond)


def evaluate(payload, flt):
    if flt is None:
        return True
    ok = True
    if "must" in flt:
        ok = ok and all(condition(payload, c) for c in flt["must"])
    if "should" in flt:
        ok = ok and any(condition(payload, c) for c in flt["should"])
    return ok


def select(payload, with_payload):
    if with_payload is False:
        return None
    if isinstance(with_payload, dict) and "include" in with_payload:
        out = {}
        for path in with_payload["include"]:
            top = path.split(".")[0]
            if top in payload:
                out[top] = payload[top]
        return out
    return payload


def id_key(point_id):
    return (isinstance(point_id, str), point_id)


def point(p, with_payload, extra):
    out = {"id": p["id"]}
    out.update(extra)
    selected = select(p["payload"], with_payload)
    if selected is not None:
        out["payload"] = selected
    return out


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def reply(self, status, body, headers=()):
        data = body if isinstance(body, bytes) else json.dumps(body).encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(data)))
        for name, value in headers:
            self.send_header(name, value)
        self.end_headers()
        self.wfile.write(data)

    def do_POST(self):
        raw = self.rfile.read(int(self.headers.get("Content-Length", "0")))
        with LOCK:
            with open(ARGS.log, "a") as log:
                log.write("POST " + self.path + "\n")
        if self.headers.get("api-key") != ARGS.api_key:
            return self.reply(401, {"status": {"error": "Unauthorized"}, "time": 0})
        parts = self.path.split("/")
        if len(parts) != 5 or parts[1] != "collections" or parts[3] != "points" or parts[4] not in ("scroll", "query"):
            return self.reply(404, {"status": {"error": "Not found"}, "time": 0})
        name, op = parts[2], parts[4]
        if name == "redirect":
            return self.reply(302, b"", [("Location", "https://example.invalid/")])
        if name == "slow":
            time.sleep(2)
        if name == "big":
            return self.reply(200, b" " * 65536)
        if name == "fail503":
            return self.reply(503, {"status": {"error": "busy"}, "time": 0})
        if name == "flaky":
            with LOCK:
                FLAKY[0] += 1
                first = FLAKY[0] == 1
            if first:
                return self.reply(503, {"status": {"error": "busy"}, "time": 0})
        request = json.loads(raw)
        candidates = [p for p in POINTS if evaluate(p["payload"], request.get("filter"))]
        with_payload = request.get("with_payload", True)
        limit = request.get("limit", 10)
        if op == "scroll":
            candidates.sort(key=lambda p: id_key(p["id"]))
            offset = request.get("offset")
            if offset is not None:
                candidates = [p for p in candidates if id_key(p["id"]) >= id_key(offset)]
            page, rest = candidates[:limit], candidates[limit:]
            result = {"points": [point(p, with_payload, {}) for p in page],
                      "next_page_offset": rest[0]["id"] if rest else None}
        else:
            result = {"points": [point(p, with_payload, {"version": 1, "score": 1.0 / (i + 1)})
                                 for i, p in enumerate(candidates[:limit])]}
        self.reply(200, {"result": result, "status": "ok", "time": 0.001})


class StubServer(ThreadingHTTPServer):
    def server_bind(self):
        # HTTPServer.server_bind resolves the bound address back to a host name (socket.getfqdn), a
        # reverse DNS lookup that takes seconds on some CI runners (the macOS Intel one). The stub
        # never uses that name, so it binds without the lookup.
        socketserver.TCPServer.server_bind(self)
        self.server_name = "localhost"
        self.server_port = self.server_address[1]


def main():
    global ARGS, POINTS
    parser = argparse.ArgumentParser()
    for flag in ("--cert", "--key", "--port-file", "--log", "--api-key", "--points"):
        parser.add_argument(flag, required=True)
    ARGS = parser.parse_args()
    with open(ARGS.points) as f:
        POINTS = json.load(f)
    server = StubServer(("127.0.0.1", 0), Handler)
    context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    context.load_cert_chain(ARGS.cert, ARGS.key)
    server.socket = context.wrap_socket(server.socket, server_side=True)
    with open(ARGS.port_file + ".tmp", "w") as f:
        f.write(str(server.server_address[1]))
    os.replace(ARGS.port_file + ".tmp", ARGS.port_file)
    server.serve_forever()


if __name__ == "__main__":
    main()
