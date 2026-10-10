#!/usr/bin/env python3
"""Lumen Studio's agent inside a rented RunPod GPU pod (M15, cloud LoRA training).

Standard library only. Lumen passes this file to the pod in the LUMEN_AGENT environment variable
and talks to it through RunPod's HTTPS proxy, where every connection ends after 100 seconds, so
every request is short: files move in chunks and progress is polled.

Environment:
  LUMEN_TOKEN          every request must send "Authorization: Bearer <token>"
  LUMEN_PORT           HTTP port (default 8000)
  LUMEN_HOST           address to listen on (default 0.0.0.0; tests use 127.0.0.1)
  LUMEN_WORK           working folder (default /workspace/lumen)
  LUMEN_IDLE_MINUTES   exit when nobody asked anything for this long and nothing runs (default 60)
  LUMEN_MAX_HOURS      exit after this long in any case, stopping a training (default 8)

API (JSON unless noted):
  GET  /status                     phase, setup, fetches, training, files, uptime
  PUT  /files/<name>?offset=N      raw chunk; N must equal the file's current size (else 409 + size)
  POST /check {name, sha256}       compares a file's SHA-256
  POST /fetch {name, url, sha256}  downloads a file in the background (checked against its SHA-256)
  POST /setup {...}                installs the trainer in the background (see Setup)
  POST /unzip {name, to}           extracts a zip from files/ into a folder of the work dir
  POST /train {args}               runs the trainer: [python] + args, "{work}" replaced
  GET  /log?offset=N               the training log from byte N (raw bytes)
  GET  /outputs                    files in output/ with sizes and SHA-256
  GET  /download?path=P&offset=N&length=L   raw bytes of a file under the work dir
  POST /cancel                     stops the training
  POST /shutdown                   exits
"""

import hashlib
import hmac
import json
import os
import shutil
import signal
import subprocess
import sys
import tarfile
import threading
import time
import urllib.parse
import urllib.request
import zipfile
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

TOKEN = os.environ.get("LUMEN_TOKEN", "")
PORT = int(os.environ.get("LUMEN_PORT", "8000"))
HOST = os.environ.get("LUMEN_HOST", "0.0.0.0")
WORK = os.path.abspath(os.environ.get("LUMEN_WORK", "/workspace/lumen"))
IDLE_SECONDS = float(os.environ.get("LUMEN_IDLE_MINUTES", "60")) * 60
MAX_SECONDS = float(os.environ.get("LUMEN_MAX_HOURS", "8")) * 3600
MAX_CHUNK = 64 * 1024 * 1024
STARTED = time.time()

FILES = os.path.join(WORK, "files")
LOG = os.path.join(WORK, "train.log")
SETUP_LOG = os.path.join(WORK, "setup.log")

lock = threading.Lock()
state = {
    "setup": {"state": "waiting", "message": ""},  # waiting, running, ready, failed
    "fetch": {},  # name -> {state, received, total, error}
    "training": {"state": "idle", "exit_code": None, "error": ""},  # idle, running, done, failed, cancelled
}
last_request = time.time()
trainer = None  # subprocess.Popen
python_exe = None  # the venv's python once set up
scripts_dir = None  # sd-scripts once set up
sha_cache = {}  # path -> (mtime, size, sha256)


def log_setup(text):
    with open(SETUP_LOG, "a", encoding="utf-8") as f:
        f.write(text.rstrip() + "\n")


def safe_path(rel):
    """A path under WORK; None when it would leave it."""
    path = os.path.abspath(os.path.join(WORK, rel.replace("\\", "/")))
    return path if path == WORK or path.startswith(WORK + os.sep) else None


def file_name_ok(name):
    return bool(name) and all(c.isalnum() or c in "._-" for c in name) and not name.startswith(".")


def sha256_of(path):
    st = os.stat(path)
    cached = sha_cache.get(path)
    if cached and cached[0] == st.st_mtime and cached[1] == st.st_size:
        return cached[2]
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    digest = h.hexdigest()
    sha_cache[path] = (st.st_mtime, st.st_size, digest)
    return digest


def download(url, target, sha256, progress=None, headers=None):
    """Downloads url to target, checks its SHA-256 (when given). Raises on failure."""
    tmp = target + ".part"
    req = urllib.request.Request(url, headers=dict(headers or {}, **{"User-Agent": "LumenStudio-PodAgent"}))
    h = hashlib.sha256()
    with urllib.request.urlopen(req, timeout=120) as r, open(tmp, "wb") as f:
        total = int(r.headers.get("Content-Length") or 0)
        received = 0
        while True:
            block = r.read(1 << 20)
            if not block:
                break
            f.write(block)
            h.update(block)
            received += len(block)
            if progress:
                progress(received, total)
    if sha256 and h.hexdigest().lower() != sha256.lower():
        os.remove(tmp)
        raise RuntimeError("checksum mismatch for %s" % url)
    os.replace(tmp, target)


def run_logged(cmd, cwd=None, env=None):
    log_setup("$ " + " ".join(cmd))
    with open(SETUP_LOG, "a", encoding="utf-8") as out:
        p = subprocess.run(cmd, cwd=cwd, env=env, stdout=out, stderr=subprocess.STDOUT)
    if p.returncode != 0:
        raise RuntimeError("%s failed with exit code %d" % (os.path.basename(cmd[0]), p.returncode))


def setup_message(text):
    with lock:
        state["setup"]["message"] = text
    log_setup(text)


def run_setup(recipe):
    """Setup: uv (pinned release) → sd-scripts (pinned archive) → Python via uv → venv → hashed lock → check.
    {"skip": true} (tests) uses this Python and the files/ folder as the scripts folder."""
    global python_exe, scripts_dir
    try:
        if recipe.get("skip"):
            python_exe, scripts_dir = sys.executable, FILES
        else:
            tools = os.path.join(WORK, "tools")
            os.makedirs(tools, exist_ok=True)
            uv = recipe["uv"]
            setup_message("Downloading uv")
            archive = os.path.join(tools, "uv.tar.gz")
            download(uv["url"], archive, uv["sha256"])
            with tarfile.open(archive) as t:
                t.extractall(tools, filter="data") if sys.version_info >= (3, 12) else t.extractall(tools)
            uv_exe = next(os.path.join(root, "uv") for root, _, names in os.walk(tools) if "uv" in names)
            os.chmod(uv_exe, 0o755)

            source = recipe["source"]
            setup_message("Downloading the trainer's source")
            zpath = os.path.join(tools, "sd-scripts.zip")
            download(source["url"], zpath, source["sha256"])
            with zipfile.ZipFile(zpath) as z:
                z.extractall(tools)
            target = os.path.join(WORK, "sd-scripts")
            shutil.rmtree(target, ignore_errors=True)
            os.replace(os.path.join(tools, source["root"]), target)

            env = dict(os.environ, UV_PYTHON_INSTALL_DIR=os.path.join(tools, "python"), UV_CACHE_DIR=os.path.join(tools, "uv-cache"))
            venv = os.path.join(WORK, "venv")
            setup_message("Installing Python %s" % recipe["python"])
            run_logged([uv_exe, "venv", "--python", recipe["python"], venv], env=env)
            python = os.path.join(venv, "bin", "python")
            lock_file = os.path.join(FILES, recipe.get("lock", "lock.txt"))
            setup_message("Installing PyTorch and the trainer's packages")
            run_logged([uv_exe, "pip", "install", "--python", python, "--require-hashes", "--no-deps",
                        "--index-strategy", "unsafe-best-match", "-r", lock_file], env=env)
            setup_message("Checking PyTorch and the GPU")
            check = subprocess.run([python, "-c", "import torch, library.train_util; print('LUMEN', torch.__version__, torch.cuda.is_available())"],
                                   cwd=target, capture_output=True, text=True)
            log_setup(check.stdout + check.stderr)
            if "LUMEN" not in check.stdout or "True" not in check.stdout.split("LUMEN", 1)[1]:
                raise RuntimeError("PyTorch does not see the GPU: " + (check.stdout + check.stderr).strip()[-400:])
            python_exe, scripts_dir = python, target
        with lock:
            state["setup"] = {"state": "ready", "message": ""}
    except Exception as e:  # noqa: BLE001 - reported to Lumen
        log_setup("FAILED: %s" % e)
        with lock:
            state["setup"] = {"state": "failed", "message": str(e)}


def run_fetch(name, url, sha256, headers):
    def progress(received, total):
        with lock:
            state["fetch"][name].update(received=received, total=total)
    try:
        download(url, os.path.join(FILES, name), sha256, progress, headers)
        with lock:
            state["fetch"][name]["state"] = "done"
    except Exception as e:  # noqa: BLE001
        with lock:
            state["fetch"][name].update(state="failed", error=str(e))


def watch_trainer(proc, out):
    code = proc.wait()
    out.close()
    with lock:
        if state["training"]["state"] == "running":
            state["training"].update(state="done" if code == 0 else "failed", exit_code=code)


def status():
    files = {}
    if os.path.isdir(FILES):
        for name in os.listdir(FILES):
            p = os.path.join(FILES, name)
            if os.path.isfile(p):
                files[name] = os.path.getsize(p)
    with lock:
        return {
            "agent": 1,
            "setup": dict(state["setup"]),
            "fetch": json.loads(json.dumps(state["fetch"])),
            "training": dict(state["training"]),
            "files": files,
            "log_size": os.path.getsize(LOG) if os.path.exists(LOG) else 0,
            "uptime": int(time.time() - STARTED),
        }


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, fmt, *args):  # quiet
        pass

    def reply(self, code, body=b"", content_type="application/json"):
        if isinstance(body, (dict, list)):
            body = json.dumps(body).encode()
        self.send_response(code)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def error(self, code, message):
        self.reply(code, {"error": message})

    def authorized(self):
        global last_request
        auth = self.headers.get("Authorization", "")
        ok = bool(TOKEN) and hmac.compare_digest(auth, "Bearer " + TOKEN)
        if ok:
            last_request = time.time()
        else:
            self.error(401, "unauthorized")
        return ok

    def body(self):
        n = int(self.headers.get("Content-Length") or 0)
        return self.rfile.read(n) if n else b""

    def json_body(self):
        try:
            return json.loads(self.body() or b"{}")
        except ValueError:
            return {}

    def do_GET(self):
        if not self.authorized():
            return
        url = urllib.parse.urlsplit(self.path)
        q = dict(urllib.parse.parse_qsl(url.query))
        if url.path == "/status":
            return self.reply(200, status())
        if url.path == "/log":
            offset = int(q.get("offset", "0"))
            if not os.path.exists(LOG):
                return self.reply(200, b"", "application/octet-stream")
            with open(LOG, "rb") as f:
                f.seek(offset)
                return self.reply(200, f.read(256 * 1024), "application/octet-stream")
        if url.path == "/outputs":
            out = os.path.join(WORK, "output")
            items = []
            if os.path.isdir(out):
                for name in sorted(os.listdir(out)):
                    p = os.path.join(out, name)
                    if os.path.isfile(p) and name.endswith(".safetensors"):
                        items.append({"path": "output/" + name, "size": os.path.getsize(p), "sha256": sha256_of(p)})
            return self.reply(200, {"outputs": items})
        if url.path == "/download":
            path = safe_path(q.get("path", ""))
            if not path or not os.path.isfile(path):
                return self.error(404, "no such file")
            offset = int(q.get("offset", "0"))
            length = min(int(q.get("length", str(MAX_CHUNK))), MAX_CHUNK)
            with open(path, "rb") as f:
                f.seek(offset)
                return self.reply(200, f.read(length), "application/octet-stream")
        self.error(404, "unknown request")

    def do_PUT(self):
        if not self.authorized():
            return
        url = urllib.parse.urlsplit(self.path)
        q = dict(urllib.parse.parse_qsl(url.query))
        if not url.path.startswith("/files/"):
            return self.error(404, "unknown request")
        name = urllib.parse.unquote(url.path[len("/files/"):])
        if not file_name_ok(name):
            return self.error(400, "bad file name")
        n = int(self.headers.get("Content-Length") or 0)
        if n > MAX_CHUNK:
            return self.error(413, "chunk too large")
        os.makedirs(FILES, exist_ok=True)
        path = os.path.join(FILES, name)
        size = os.path.getsize(path) if os.path.exists(path) else 0
        offset = int(q.get("offset", "0"))
        data = self.rfile.read(n) if n else b""
        if offset == 0 and size:
            size = 0
            open(path, "wb").close()  # a new upload replaces the file
        if offset != size:
            return self.reply(409, {"error": "offset mismatch", "size": size})
        with open(path, "ab") as f:
            f.write(data)
        self.reply(200, {"size": size + len(data)})

    def do_POST(self):
        global trainer
        if not self.authorized():
            return
        path = urllib.parse.urlsplit(self.path).path
        req = self.json_body()
        if path == "/check":
            name = req.get("name", "")
            p = os.path.join(FILES, name)
            if not file_name_ok(name) or not os.path.isfile(p):
                return self.error(404, "no such file")
            digest = sha256_of(p)
            return self.reply(200, {"sha256": digest, "ok": digest.lower() == str(req.get("sha256", "")).lower()})
        if path == "/fetch":
            name = req.get("name", "")
            if not file_name_ok(name) or not req.get("url"):
                return self.error(400, "name and url are needed")
            os.makedirs(FILES, exist_ok=True)
            with lock:
                state["fetch"][name] = {"state": "running", "received": 0, "total": 0, "error": ""}
            threading.Thread(target=run_fetch, args=(name, req["url"], req.get("sha256"), req.get("headers") or {}), daemon=True).start()
            return self.reply(202, {"started": name})
        if path == "/setup":
            with lock:
                if state["setup"]["state"] in ("running", "ready"):
                    return self.reply(200, {"setup": state["setup"]["state"]})
                state["setup"] = {"state": "running", "message": "Starting"}
            threading.Thread(target=run_setup, args=(req,), daemon=True).start()
            return self.reply(202, {"setup": "running"})
        if path == "/unzip":
            name, to = req.get("name", ""), req.get("to", "img")
            src, dest = os.path.join(FILES, name), safe_path(to)
            if not file_name_ok(name) or not os.path.isfile(src) or not dest or dest == WORK:
                return self.error(400, "bad unzip request")
            shutil.rmtree(dest, ignore_errors=True)
            os.makedirs(dest)
            with zipfile.ZipFile(src) as z:
                for member in z.namelist():
                    target = os.path.abspath(os.path.join(dest, member))
                    if not target.startswith(dest + os.sep):
                        return self.error(400, "unsafe path in zip: " + member)
                z.extractall(dest)
            return self.reply(200, {"extracted": len(os.listdir(dest))})
        if path == "/train":
            args = req.get("args") or []
            with lock:
                if state["setup"]["state"] != "ready":
                    return self.error(409, "the trainer is not set up")
                if state["training"]["state"] == "running":
                    return self.error(409, "a training is running")
            # "{work}" stands for the work folder; Lumen builds the arguments on Windows.
            args = [a.replace("{work}", WORK).replace("\\", "/") if "{work}" in a else a for a in args]
            os.makedirs(os.path.join(WORK, "output"), exist_ok=True)
            out = open(LOG, "wb")
            env = dict(os.environ, PYTHONUNBUFFERED="1")
            trainer = subprocess.Popen([python_exe] + args, cwd=scripts_dir, stdout=out, stderr=subprocess.STDOUT, env=env)
            with lock:
                state["training"] = {"state": "running", "exit_code": None, "error": ""}
            threading.Thread(target=watch_trainer, args=(trainer, out), daemon=True).start()
            return self.reply(202, {"training": "running"})
        if path == "/cancel":
            with lock:
                running = state["training"]["state"] == "running"
                if running:
                    state["training"]["state"] = "cancelled"
            if running and trainer:
                trainer.terminate()
            return self.reply(200, {"cancelled": running})
        if path == "/shutdown":
            self.reply(200, {"bye": True})
            threading.Timer(0.5, stop_and_exit).start()
            return None
        self.error(404, "unknown request")


def stop_and_exit():
    if trainer and trainer.poll() is None:
        trainer.terminate()
    os._exit(0)


def watchdog():
    while True:
        time.sleep(30)
        now = time.time()
        with lock:
            busy = state["training"]["state"] == "running" or state["setup"]["state"] == "running" or any(
                f["state"] == "running" for f in state["fetch"].values())
        if now - STARTED > MAX_SECONDS or (not busy and now - last_request > IDLE_SECONDS):
            stop_and_exit()


def main():
    if not TOKEN:
        print("LUMEN_TOKEN is not set", file=sys.stderr)
        sys.exit(2)
    os.makedirs(FILES, exist_ok=True)
    threading.Thread(target=watchdog, daemon=True).start()
    server = ThreadingHTTPServer((HOST, PORT), Handler)
    print("Lumen pod agent on port %d, work %s" % (PORT, WORK), flush=True)
    if hasattr(signal, "SIGTERM"):
        signal.signal(signal.SIGTERM, lambda *_: stop_and_exit())
    server.serve_forever()


if __name__ == "__main__":
    main()
