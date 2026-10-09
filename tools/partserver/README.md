# Lumen Part Server

A small HTTP sidecar that lets Lumen Studio segment a character illustration into semantic
parts for Live2D slicing (the **Parts** page). The reference implementation wraps
[see-through](https://github.com/shitagaki-lab/see-through). see-through uses LayerDiff 3D,
Marigold depth and a body-parsing SAM to produce up to 23 transparent layers, with hidden
areas already completed.

## Setup

1. Install see-through by following its README (Python 3.12, PyTorch with CUDA; 12–16 GB of VRAM,
   or 8 GB with the quantized or block-swap scripts). Its models download on first run.
2. In the Python environment you use for the server (it can be the same one), install:

   ```bash
   pip install pillow psd-tools
   ```

3. Start the server:

   ```bash
   python tools/partserver/server.py --see-through C:/path/to/see-through --python C:/path/to/see-through/.venv/Scripts/python.exe
   ```

   Useful options:
   - `--script inference/scripts/inference_psd_quantized.py`: NF4 model for 8 GB GPUs.
   - `--extra "--group_offload"`: lower VRAM for the full model.
   - `--port 7870`: default port; Lumen detects it automatically.
   - `--mock`: serves simple test layers without a model, to check the connection.

4. In Lumen Studio, open **Settings → Providers → Add → Lumen Part Server** (or use *Detect local
   servers*), then press *Test connection*.

## Protocol

All bodies are JSON. Images are base64 PNG.

| Request | Response |
|---|---|
| `GET /v1/info` | `{"name", "version", "model", "tags": [...], "capabilities": ["segment"]}` |
| `POST /v1/segment` with `{"image", "parts": [{"tag", "prompt"}], "options": {"resolution", "seed"}}` | `{"width", "height", "layers": [{"tag", "image", "x", "y", "kind": "layer" or "mask", "order", "depth", "score"}], "info": {}}` |

Response fields:
* `layers` may be cropped. `x` and `y` place them on a `width` × `height` canvas, which Lumen
  scales to the source image when the sizes differ.
* `kind: "layer"` is an RGBA layer whose hidden areas may already be completed. Lumen resolves
  visibility by `order` (back to front).
* `kind: "mask"` is a grayscale mask.
* Tags are matched to Lumen's part taxonomy, including aliases such as "bangs" → front hair. A
  tag Lumen doesn't recognise becomes a custom part.

Errors use HTTP 4xx or 5xx with `{"error": "message"}`.

Any segmentation model can be put behind this protocol. It only needs to return labelled masks or
layers.
