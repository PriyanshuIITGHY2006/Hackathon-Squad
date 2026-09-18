#!/usr/bin/env python3
"""Backend for the MWIS engine workbench.

Runs the real C++ engine as a child process and streams its JSONL trace to the
browser over Server-Sent Events, so the page shows the actual solve as it
happens rather than a replay.  Standard library only — no pip install.

    python3 server/server.py [--port 8000] [--host 127.0.0.1]

Endpoints
    GET  /                     the web app
    GET  /api/health           build status, engine path, available generators
    POST /api/generate         {kind,n,seed,density,wmax} -> graph
    POST /api/run              {graph|text, options} -> {run_id, cmd}
    GET  /api/events/<id>      SSE stream of engine events (?from=N to resume)
    POST /api/stop/<id>        SIGTERM the run; it still emits a verified answer
    POST /api/exact            {graph} -> proven optimum (n <= 64)
    POST /api/compare          {graph} -> engine vs solution.cpp vs exact
    POST /api/verify           {graph, vertices} -> independent re-check
"""

import argparse
import json
import os
import signal
import subprocess
import sys
import threading
import time
import uuid
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
WEB = os.path.join(ROOT, "web")
BIN = os.path.join(ROOT, "bin")
ENGINE = os.path.join(BIN, "mwis_engine")
EXACT = os.path.join(BIN, "mwis_exact")
LEGACY = os.path.join(BIN, "mwis_legacy")

sys.path.insert(0, ROOT)
from tools import graphs  # noqa: E402

MAX_BODY = 64 * 1024 * 1024
MAX_N = 200000
MAX_M = 2000000
MAX_RUNS = 32

BUILD_LOG = []


# ── build ────────────────────────────────────────────────────────────────────
def ensure_built(force=False):
    """Compile the engine if a binary is missing or older than its source."""
    global BUILD_LOG
    need = force
    for binary, src in ((ENGINE, "engine/mwis_engine.cpp"),
                        (EXACT, "engine/mwis_exact.cpp"),
                        (LEGACY, "solution.cpp")):
        srcp = os.path.join(ROOT, src)
        if not os.path.exists(binary):
            need = True
        elif os.path.exists(srcp) and os.path.getmtime(srcp) > os.path.getmtime(binary):
            need = True
    if not need:
        return True, "up to date"
    proc = subprocess.run(["make", "-C", os.path.join(ROOT, "engine")],
                          capture_output=True, text=True)
    BUILD_LOG = (proc.stdout + proc.stderr).strip().splitlines()[-40:]
    return proc.returncode == 0, "\n".join(BUILD_LOG)


# ── run registry ─────────────────────────────────────────────────────────────
class Run:
    """One engine process plus the event log the browser tails."""

    def __init__(self, run_id, cmd, input_text):
        self.id = run_id
        self.cmd = cmd
        self.events = []
        self.stderr = []
        self.finished = False
        self.returncode = None
        self.started = time.time()
        self.cond = threading.Condition()
        self.proc = subprocess.Popen(
            cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=subprocess.PIPE, text=True, bufsize=1,
            start_new_session=True)
        threading.Thread(target=self._feed, args=(input_text,), daemon=True).start()
        threading.Thread(target=self._pump, daemon=True).start()
        threading.Thread(target=self._pump_err, daemon=True).start()

    def _feed(self, text):
        try:
            self.proc.stdin.write(text)
            self.proc.stdin.close()
        except Exception:
            pass

    def _append(self, line):
        with self.cond:
            self.events.append(line)
            self.cond.notify_all()

    def _pump(self):
        try:
            for line in self.proc.stdout:
                line = line.strip()
                if line:
                    self._append(line)
        except Exception as exc:
            self._append(json.dumps({"e": "error", "msg": "stdout closed: %s" % exc}))
        self.proc.wait()
        with self.cond:
            self.returncode = self.proc.returncode
            self.finished = True
            self.cond.notify_all()

    def _pump_err(self):
        try:
            for line in self.proc.stderr:
                line = line.rstrip()
                if line:
                    self.stderr.append(line)
                    self._append(json.dumps({"e": "stderr", "msg": line}))
        except Exception:
            pass

    def stop(self):
        if self.proc.poll() is None:
            try:
                self.proc.send_signal(signal.SIGTERM)
                return True
            except Exception:
                return False
        return False

    def kill(self):
        if self.proc.poll() is None:
            try:
                self.proc.kill()
            except Exception:
                pass


RUNS = {}
RUNS_LOCK = threading.Lock()


def register(run):
    with RUNS_LOCK:
        RUNS[run.id] = run
        if len(RUNS) > MAX_RUNS:                       # reap the oldest finished runs
            for rid, r in sorted(RUNS.items(), key=lambda kv: kv[1].started):
                if len(RUNS) <= MAX_RUNS:
                    break
                if r.finished:
                    del RUNS[rid]


# ── option handling (strict whitelist: nothing reaches argv unvalidated) ─────
def build_args(options):
    def num(key, default, lo, hi, cast=int):
        try:
            val = cast(options.get(key, default))
        except (TypeError, ValueError):
            val = default
        return max(lo, min(hi, val))

    args = ["--trace"]
    args += ["--seed", str(num("seed", 12345, 0, 2 ** 31 - 1))]
    args += ["--stall", str(num("stall", 2000, 0, 10 ** 9))]
    args += ["--exact-threshold", str(num("exact_threshold", 100, 0, 256))]
    args += ["--exact-nodes", str(num("exact_nodes", 3000000, 1000, 10 ** 9))]
    args += ["--max-events", str(num("max_events", 4000, 0, 200000))]
    tl = num("time_limit", 0, 0, 86400, float)
    if tl > 0:
        args += ["--time-limit", str(tl)]
    mi = num("max_iters", 0, 0, 10 ** 9)
    if mi > 0:
        args += ["--max-iters", str(mi)]
    if options.get("no_reduce"):
        args.append("--no-reduce")
    if options.get("legacy_rules"):
        args.append("--legacy-rules")
    disabled = options.get("disable") or []
    allowed = {"R0", "R1", "R2", "R3", "R4", "R5", "R6", "R7"}
    picked = [d for d in disabled if d in allowed]
    if picked:
        args += ["--disable", ",".join(picked)]
    return args


def graph_from_payload(payload):
    """Accept either a structured graph or raw engine-format text."""
    if payload.get("text"):
        g = graphs.parse_engine_text(payload["text"])
    else:
        g = payload.get("graph") or {}
        g = {"n": int(g.get("n", 0)),
             "weights": [int(w) for w in g.get("weights", [])],
             "edges": [(int(u), int(v)) for u, v in g.get("edges", [])],
             "kind": g.get("kind", "custom"), "seed": g.get("seed", 0)}
    n = g["n"]
    if n < 1 or n > MAX_N:
        raise ValueError("n must be between 1 and %d" % MAX_N)
    if len(g["weights"]) != n:
        raise ValueError("expected %d weights, got %d" % (n, len(g["weights"])))
    if len(g["edges"]) > MAX_M:
        raise ValueError("too many edges")
    for u, v in g["edges"]:
        if not (1 <= u <= n and 1 <= v <= n):
            raise ValueError("edge endpoint out of range")
    return g


def run_blocking(path, text, args=(), timeout=30.0):
    proc = subprocess.Popen([path] + list(args), stdin=subprocess.PIPE,
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                            text=True, start_new_session=True)
    try:
        out, err = proc.communicate(text, timeout=timeout)
        return proc.returncode, out, err, False
    except subprocess.TimeoutExpired:
        proc.terminate()
        try:
            out, err = proc.communicate(timeout=10)
        except subprocess.TimeoutExpired:
            proc.kill()
            out, err = proc.communicate()
        return proc.returncode, out, err, True


def parse_plain(out):
    lines = [ln for ln in (out or "").strip().splitlines() if ln.strip()]
    if not lines:
        return None, []
    try:
        weight = int(lines[0].strip())
    except ValueError:
        return None, []
    verts = [int(x) for x in lines[1].split()] if len(lines) > 1 else []
    return weight, verts


# ── HTTP ─────────────────────────────────────────────────────────────────────
CONTENT_TYPES = {
    ".html": "text/html; charset=utf-8",
    ".js": "application/javascript; charset=utf-8",
    ".css": "text/css; charset=utf-8",
    ".svg": "image/svg+xml",
    ".json": "application/json",
    ".ico": "image/x-icon",
}


class Handler(BaseHTTPRequestHandler):
    server_version = "mwis-workbench"
    protocol_version = "HTTP/1.1"

    def log_message(self, fmt, *args):
        if os.environ.get("MWIS_VERBOSE"):
            sys.stderr.write("%s - %s\n" % (self.address_string(), fmt % args))

    # -- helpers --
    def send_json(self, obj, code=200):
        body = json.dumps(obj).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def read_json(self):
        length = int(self.headers.get("Content-Length") or 0)
        if length <= 0 or length > MAX_BODY:
            return {}
        return json.loads(self.rfile.read(length).decode())

    def serve_file(self, relpath):
        safe = os.path.normpath(relpath).lstrip("/")
        path = os.path.join(WEB, safe)
        if not os.path.abspath(path).startswith(os.path.abspath(WEB)) or not os.path.isfile(path):
            self.send_error(404, "not found")
            return
        ext = os.path.splitext(path)[1]
        with open(path, "rb") as fh:
            body = fh.read()
        self.send_response(200)
        self.send_header("Content-Type", CONTENT_TYPES.get(ext, "application/octet-stream"))
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    # -- routes --
    def do_GET(self):
        path = self.path.split("?", 1)[0]
        query = {}
        if "?" in self.path:
            for part in self.path.split("?", 1)[1].split("&"):
                if "=" in part:
                    k, v = part.split("=", 1)
                    query[k] = v

        if path == "/" or path == "/index.html":
            return self.serve_file("index.html")
        if path.startswith("/static/"):
            return self.serve_file(path[len("/static/"):])
        if path == "/api/health":
            ok, log = ensure_built()
            return self.send_json({
                "ok": ok, "build": log, "engine": ENGINE,
                "engine_exists": os.path.exists(ENGINE),
                "exact_exists": os.path.exists(EXACT),
                "legacy_exists": os.path.exists(LEGACY),
                "kinds": graphs.KINDS,
                "runs": len(RUNS),
            })
        if path.startswith("/api/events/"):
            return self.stream_events(path.rsplit("/", 1)[-1], int(query.get("from", 0)))
        return self.send_error(404, "not found")

    def do_POST(self):
        path = self.path.split("?", 1)[0]
        try:
            payload = self.read_json()
        except Exception as exc:
            return self.send_json({"error": "bad JSON: %s" % exc}, 400)

        try:
            if path == "/api/generate":
                kind = payload.get("kind", "random")
                if kind not in graphs.KINDS:
                    return self.send_json({"error": "unknown kind"}, 400)
                n = max(1, min(int(payload.get("n", 40)), 20000))
                seed = int(payload.get("seed", 1)) & 0x7FFFFFFF
                density = float(payload.get("density", 0.12))
                density = max(0.0, min(1.0, density))
                wmax = max(1, min(int(payload.get("wmax", 100)), 10 ** 6))
                g = graphs.generate(kind=kind, n=n, seed=seed, density=density, wmax=wmax)
                return self.send_json({"graph": {"n": g["n"], "weights": g["weights"],
                                                 "edges": [list(e) for e in g["edges"]],
                                                 "kind": g["kind"], "seed": g["seed"]}})

            if path == "/api/run":
                ok, log = ensure_built()
                if not ok:
                    return self.send_json({"error": "build failed", "build": log}, 500)
                g = graph_from_payload(payload)
                args = build_args(payload.get("options") or {})
                run_id = uuid.uuid4().hex[:12]
                cmd = [ENGINE] + args
                run = Run(run_id, cmd, graphs.to_engine_text(g))
                register(run)
                return self.send_json({"run_id": run_id,
                                       "cmd": " ".join(["bin/mwis_engine"] + args),
                                       "n": g["n"], "m": len(g["edges"])})

            if path.startswith("/api/stop/"):
                run = RUNS.get(path.rsplit("/", 1)[-1])
                if not run:
                    return self.send_json({"error": "unknown run"}, 404)
                return self.send_json({"stopped": run.stop()})

            if path == "/api/exact":
                ensure_built()
                g = graph_from_payload(payload)
                if g["n"] > 64:
                    return self.send_json({"error": "exact certification is limited to 64 vertices",
                                           "n": g["n"]}, 400)
                budget = max(1.0, min(float(payload.get("timeout", 20)), 120.0))
                rc, out, err, timed_out = run_blocking(EXACT, graphs.to_engine_text(g), timeout=budget)
                weight, verts = parse_plain(out)
                if weight is None:
                    return self.send_json({"error": "exact solver produced no answer",
                                           "timed_out": timed_out, "stderr": err[-400:]}, 504)
                nodes = 0
                for line in (err or "").splitlines():
                    if line.startswith("nodes="):
                        nodes = int(line.split("=")[1])
                return self.send_json({"weight": weight, "vertices": verts,
                                       "nodes": nodes, "timed_out": timed_out})

            if path == "/api/compare":
                ensure_built()
                g = graph_from_payload(payload)
                text = graphs.to_engine_text(g)
                out_obj = {"n": g["n"], "m": len(g["edges"])}

                rc, out, err, _ = run_blocking(ENGINE, text, args=["--stall", "600"], timeout=60)
                w, verts = parse_plain(out)
                ok, actual, conf = graphs.verify(g, verts)
                out_obj["engine"] = {"weight": actual, "claimed": w, "valid": ok,
                                     "conflicts": conf, "size": len(verts)}

                rc, out, err, _ = run_blocking(ENGINE, text,
                                               args=["--legacy-rules", "--stall", "600"], timeout=60)
                w, verts = parse_plain(out)
                ok, actual, conf = graphs.verify(g, verts)
                out_obj["legacy_rules"] = {"weight": actual, "claimed": w, "valid": ok,
                                           "conflicts": conf, "size": len(verts)}

                budget = max(0.5, min(float(payload.get("legacy_budget", 2.0)), 30.0))
                rc, out, err, timed_out = run_blocking(LEGACY, text, timeout=budget)
                w, verts = parse_plain(out)
                ok, actual, conf = graphs.verify(g, verts)
                out_obj["solution_cpp"] = {"weight": actual, "claimed": w, "valid": ok,
                                           "conflicts": conf, "size": len(verts),
                                           "stopped_early": timed_out}

                if g["n"] <= 64:
                    rc, out, err, timed_out = run_blocking(EXACT, text, timeout=30)
                    w, verts = parse_plain(out)
                    if w is not None and not timed_out:
                        out_obj["optimum"] = w
                return self.send_json(out_obj)

            if path == "/api/verify":
                g = graph_from_payload(payload)
                verts = [int(v) for v in (payload.get("vertices") or [])]
                ok, weight, conflicts = graphs.verify(g, verts)
                return self.send_json({"ok": ok, "weight": weight, "conflicts": conflicts,
                                       "size": len(set(verts))})

            if path == "/api/rebuild":
                ok, log = ensure_built(force=True)
                return self.send_json({"ok": ok, "build": log})

        except ValueError as exc:
            return self.send_json({"error": str(exc)}, 400)
        except Exception as exc:  # keep the workbench alive on unexpected input
            return self.send_json({"error": "%s: %s" % (type(exc).__name__, exc)}, 500)

        return self.send_error(404, "not found")

    def stream_events(self, run_id, start):
        run = RUNS.get(run_id)
        if not run:
            return self.send_error(404, "unknown run")
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.send_header("Cache-Control", "no-cache")
        self.send_header("Connection", "close")
        self.end_headers()

        idx = max(0, start)
        try:
            while True:
                with run.cond:
                    while idx >= len(run.events) and not run.finished:
                        run.cond.wait(timeout=0.5)
                    batch = run.events[idx:]
                    idx = len(run.events)
                    finished = run.finished
                    rc = run.returncode
                if batch:
                    chunk = "".join("data: %s\n\n" % line for line in batch)
                    self.wfile.write(chunk.encode())
                    self.wfile.flush()
                elif finished:
                    tail = json.dumps({"e": "end", "returncode": rc,
                                       "stderr": run.stderr[-10:]})
                    self.wfile.write(("data: %s\n\n" % tail).encode())
                    self.wfile.flush()
                    return
                else:
                    self.wfile.write(b": keepalive\n\n")
                    self.wfile.flush()
        except (BrokenPipeError, ConnectionResetError):
            return


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=8000)
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--no-build", action="store_true")
    args = ap.parse_args()

    if not args.no_build:
        ok, log = ensure_built()
        print("engine build: %s" % ("ok" if ok else "FAILED"))
        if not ok:
            print(log)
            return 1

    srv = ThreadingHTTPServer((args.host, args.port), Handler)
    srv.daemon_threads = True
    print("MWIS workbench on http://%s:%d" % (args.host, args.port))
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        print("\nshutting down")
        with RUNS_LOCK:
            for run in RUNS.values():
                run.kill()
    return 0


if __name__ == "__main__":
    sys.exit(main())
