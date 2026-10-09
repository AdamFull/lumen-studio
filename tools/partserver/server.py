#!/usr/bin/env python3
"""Lumen Part Server - reference implementation wrapping see-through.

Serves the Lumen Part Server protocol used by Lumen Studio's character slicing (Live2D):

    GET  /v1/info     -> {"name", "version", "model", "tags": [...], "capabilities": ["segment"]}
    POST /v1/segment  <- {"image": base64 PNG, "parts": [{"tag", "prompt"}], "options": {"resolution", "seed"}}
                      -> {"width", "height", "layers": [{"tag", "image": base64 PNG, "x", "y", "kind", "order"}],
                          "info": {...}}

The server runs see-through's inference script (https://github.com/shitagaki-lab/see-through) in a
subprocess, reads the layered PSD it writes and returns every layer cropped to its content.
`--mock` serves simple geometric layers instead, to test the connection without a GPU.

Requirements: Python 3.10+, Pillow, psd-tools (pip install pillow psd-tools) plus a working
see-through installation (its own environment can be selected with --python).
"""

from __future__ import annotations

import argparse
import base64
import io
import json
import logging
import re
import shutil
import subprocess
import sys
import tempfile
import threading
import time
from http import HTTPStatus
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

from PIL import Image

VERSION = "1.0"
# see-through V3 body-part tags (common/live2d/scrap_model.py).
TAGS = [
    "front hair", "back hair", "headwear", "face", "irides", "eyebrow", "eyewhite", "eyelash", "eyewear",
    "ears", "earwear", "nose", "mouth", "neck", "neckwear", "topwear", "handwear", "bottomwear", "legwear",
    "footwear", "tail", "wings", "objects",
]
MAX_BODY = 200 * 1024 * 1024

log = logging.getLogger("partserver")


def encode_png(image: Image.Image) -> str:
    buf = io.BytesIO()
    image.save(buf, format="PNG", compress_level=4)
    return base64.b64encode(buf.getvalue()).decode("ascii")


def decode_image(data: str) -> Image.Image:
    if data.startswith("data:"):
        data = data.split(",", 1)[1]
    return Image.open(io.BytesIO(base64.b64decode(data))).convert("RGBA")


def normalize_tag(name: str) -> str:
    """Layer names may carry draw-order prefixes ("03_front_hair") or suffixes ("handwear-1")."""
    tag = re.sub(r"^\d+[_\-\s]*", "", name.strip())
    tag = tag.replace("_", " ").strip().lower()
    return tag


def cropped_layer(image: Image.Image) -> tuple[Image.Image, int, int] | None:
    box = image.getchannel("A").getbbox()
    if not box:
        return None
    return image.crop(box), box[0], box[1]


class SeeThroughBackend:
    def __init__(self, repo: Path, python: str, script: str, output: str, extra: list[str], timeout: int):
        self.repo = repo
        self.python = python
        self.script = script
        self.output = output
        self.extra = extra
        self.timeout = timeout
        self.lock = threading.Lock()  # one GPU job at a time

    def info(self) -> dict:
        return {"name": "see-through (Lumen Part Server)", "version": VERSION, "model": "see-through LayerDiff 3D",
                "tags": TAGS, "capabilities": ["segment"]}

    def segment(self, image: Image.Image, parts: list[dict], options: dict) -> dict:
        with self.lock, tempfile.TemporaryDirectory(prefix="lumen_parts_") as tmp:
            src = Path(tmp) / "lumen_input.png"
            image.save(src)
            out_dir = (self.repo / self.output).resolve()
            started = time.time()
            cmd = [self.python, self.script, "--srcp", str(src), "--save_to_psd"]
            resolution = int(options.get("resolution") or 0)
            if resolution > 0:
                cmd += ["--resolution", str(resolution)]
            cmd += self.extra
            log.info("running %s", " ".join(cmd))
            proc = subprocess.run(cmd, cwd=self.repo, capture_output=True, text=True, timeout=self.timeout)
            if proc.returncode != 0:
                tail = (proc.stderr or proc.stdout or "").strip().splitlines()[-12:]
                raise RuntimeError("see-through failed:\n" + "\n".join(tail))
            psd_path = self._find_psd(out_dir, "lumen_input", started)
            return self._read_psd(psd_path, image.size)

    @staticmethod
    def _find_psd(out_dir: Path, stem: str, since: float) -> Path:
        candidates = [p for p in out_dir.rglob("*.psd") if p.stat().st_mtime >= since - 1 and "depth" not in p.stem.lower()]
        named = [p for p in candidates if p.stem.startswith(stem)]
        pool = named or candidates
        if not pool:
            raise RuntimeError(f"see-through wrote no PSD into {out_dir}")
        return max(pool, key=lambda p: p.stat().st_mtime)

    @staticmethod
    def _read_psd(path: Path, size: tuple[int, int]) -> dict:
        from psd_tools import PSDImage  # imported lazily so --mock works without psd-tools

        psd = PSDImage.open(path)
        layers = []
        order = 0
        # psd-tools iterates bottom to top.
        for layer in psd.descendants():
            if layer.is_group() or not layer.has_pixels():
                continue
            rgba = layer.topil()
            if rgba is None:
                continue
            rgba = rgba.convert("RGBA")
            crop = cropped_layer(rgba)
            if not crop:
                continue
            img, dx, dy = crop
            layers.append({"tag": normalize_tag(layer.name), "image": encode_png(img), "x": layer.left + dx,
                           "y": layer.top + dy, "kind": "layer", "order": order})
            order += 1
        if not layers:
            raise RuntimeError(f"{path.name} contains no pixel layers")
        width, height = psd.width, psd.height
        info = {"psd": path.name}
        if (width, height) != tuple(size):
            info["note"] = f"model output {width}x{height}, input {size[0]}x{size[1]}"
        return {"width": width, "height": height, "layers": layers, "info": info}


class MockBackend:
    """Splits the silhouette into horizontal bands - only for testing the connection."""

    def info(self) -> dict:
        return {"name": "Lumen Part Server (mock)", "version": VERSION, "model": "mock",
                "tags": ["face", "topwear", "bottomwear", "legwear"], "capabilities": ["segment"]}

    def segment(self, image: Image.Image, parts: list[dict], options: dict) -> dict:
        w, h = image.size
        alpha = image.getchannel("A")
        if alpha.getextrema()[0] == 255:  # opaque: treat near-white as background
            gray = image.convert("L")
            alpha = gray.point(lambda v: 0 if v > 245 else 255)
        box = alpha.getbbox() or (0, 0, w, h)
        top, bottom = box[1], box[3]
        span = max(1, bottom - top)
        bands = [("face", 0.0, 0.25), ("topwear", 0.25, 0.55), ("bottomwear", 0.55, 0.72), ("legwear", 0.72, 1.0)]
        layers = []
        for order, (tag, a, b) in enumerate(bands):
            y0, y1 = top + int(span * a), top + int(span * b)
            mask = Image.new("L", (w, h), 0)
            mask.paste(alpha.crop((0, y0, w, y1)), (0, y0))
            layer = image.copy()
            layer.putalpha(mask)
            crop = cropped_layer(layer)
            if crop:
                img, x, y = crop
                layers.append({"tag": tag, "image": encode_png(img), "x": x, "y": y, "kind": "layer", "order": order})
        return {"width": w, "height": h, "layers": layers, "info": {"mock": True}}


def make_handler(backend):
    class Handler(BaseHTTPRequestHandler):
        server_version = f"LumenPartServer/{VERSION}"

        def log_message(self, fmt, *args):
            log.info("%s %s", self.address_string(), fmt % args)

        def _json(self, status: int, payload: dict):
            body = json.dumps(payload).encode("utf-8")
            self.send_response(status)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        def do_GET(self):
            if self.path.rstrip("/") == "/v1/info":
                self._json(HTTPStatus.OK, backend.info())
            else:
                self._json(HTTPStatus.NOT_FOUND, {"error": "unknown endpoint"})

        def do_POST(self):
            if self.path.rstrip("/") != "/v1/segment":
                self._json(HTTPStatus.NOT_FOUND, {"error": "unknown endpoint"})
                return
            length = int(self.headers.get("Content-Length", "0"))
            if length <= 0 or length > MAX_BODY:
                self._json(HTTPStatus.BAD_REQUEST, {"error": "missing or too large request body"})
                return
            try:
                request = json.loads(self.rfile.read(length))
                image = decode_image(request["image"])
            except Exception as exc:  # malformed input
                self._json(HTTPStatus.BAD_REQUEST, {"error": f"invalid request: {exc}"})
                return
            try:
                started = time.time()
                result = backend.segment(image, request.get("parts") or [], request.get("options") or {})
                result.setdefault("info", {})["seconds"] = round(time.time() - started, 1)
                self._json(HTTPStatus.OK, result)
            except subprocess.TimeoutExpired:
                self._json(HTTPStatus.GATEWAY_TIMEOUT, {"error": "segmentation timed out"})
            except Exception as exc:
                log.exception("segmentation failed")
                self._json(HTTPStatus.INTERNAL_SERVER_ERROR, {"error": str(exc)})

    return Handler


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=7870)
    parser.add_argument("--see-through", type=Path, help="path to the see-through repository")
    parser.add_argument("--python", default=sys.executable, help="Python interpreter of the see-through environment")
    parser.add_argument("--script", default="inference/scripts/inference_psd.py",
                        help="inference script relative to the repository (e.g. inference_psd_quantized.py for 8 GB GPUs)")
    parser.add_argument("--output", default="workspace/layerdiff_output", help="output folder of the script")
    parser.add_argument("--extra", default="", help="extra arguments for the script, e.g. \"--group_offload\"")
    parser.add_argument("--timeout", type=int, default=1800, help="seconds per image")
    parser.add_argument("--mock", action="store_true", help="serve geometric test layers (no model)")
    args = parser.parse_args()
    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(message)s")

    if args.mock:
        backend = MockBackend()
    else:
        if not args.see_through or not (args.see_through / args.script).is_file():
            parser.error("--see-through must point to a see-through checkout containing " + args.script)
        if shutil.which(args.python) is None and not Path(args.python).is_file():
            parser.error(f"Python interpreter not found: {args.python}")
        backend = SeeThroughBackend(args.see_through.resolve(), args.python, args.script, args.output,
                                    args.extra.split() if args.extra else [], args.timeout)
    server = ThreadingHTTPServer((args.host, args.port), make_handler(backend))
    log.info("Lumen Part Server %s listening on http://%s:%d (%s)", VERSION, args.host, args.port,
             "mock" if args.mock else f"see-through at {args.see_through}")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    return 0


if __name__ == "__main__":
    sys.exit(main())
