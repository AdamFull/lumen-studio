# ONNX Runtime C API headers

`include/` holds the C API headers of ONNX Runtime 1.31.0 (commit
402979ac1bef89dcd55add41eaa7277c407599eb), from `onnxruntime-win-x64-1.31.0.zip` of
<https://github.com/microsoft/onnxruntime/releases/tag/v1.31.0>, under the MIT licence in `LICENSE`.

Lumen does not link ONNX Runtime: it downloads the runtime when a feature needs it (plan M18 §18.1,
`resources/presets/components.json`) and loads `onnxruntime.dll` at run time through
`OrtGetApiBase` (`src/ai/Onnx.cpp`), so the MinGW build needs no import library. Line endings were
converted to LF.
