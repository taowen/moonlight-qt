# AGENTS.md

This file is project memory for this local Moonlight fork.

## Project Goal

This Moonlight fork adds an experimental Acer SpatialLabs / Simulated Reality
2D-to-3D output path. The normal Moonlight renderer is not the primary target
for this work. When enabled, Moonlight receives the Sunshine video stream,
decodes it with D3D11VA, estimates a depth map with Distill-Any-Depth on the
Intel NPU through ONNX Runtime OpenVINO, renders stereo left/right views with a
D3D11 pixel shader, and sends the resulting half-SBS frame to the Acer SR DX11
weaver.

The intended test chain is:

```text
Sunshine host -> Moonlight D3D11VA decode -> Acer SR sidecar -> Acer 3D display
```

## Repository Shape

This is a recursive clone under the Arctrl workspace:

```text
C:\Apps\arctrl\moonlight-qt
```

Important local additions:

- `app/streaming/video/acer_sr_sink.h`
- `app/streaming/video/acer_sr_sink.cpp`
  - Moonlight-side integration point.
  - Enabled only when `MOONLIGHT_ACER_SR=1`.
  - Creates a separate D3D11VA decoder path, loads `moonlight-acer-sr.dll`, and
    pushes decoded D3D11 textures to the sidecar.
- `app/streaming/video/ffmpeg.cpp`
  - Instantiates `AcerSrVideoSink` when the env flag is enabled.
  - Pushes video packets to the sidecar while Moonlight is streaming.
- `app/acer_sr/`
  - Standalone CMake sidecar project.
  - Builds `moonlight-acer-sr.dll` and `moonlight-acer-sr-smoke.exe`.
  - This code owns Acer SR window/display setup, OpenVINO NPU depth inference,
    D3D11 stereo shader rendering, and SR weaver presentation.
- `scripts/run-acer-sr.ps1`
  - Launches deployed `Moonlight.exe` with Acer SR env vars.
- `scripts/run-acer-sr-smoke.ps1`
  - Runs the standalone sidecar smoke test without needing Sunshine.

## Acer SR Sidecar

The sidecar exports these C functions for Moonlight:

```text
moonlight_acer_sr_init_d3d11
moonlight_acer_sr_push_d3d11
moonlight_acer_sr_update_depth_u16
moonlight_acer_sr_shutdown
moonlight_acer_sr_last_error
```

`moonlight_acer_sr_update_depth_u16` is retained for synthetic smoke/debug depth
injection, but production streaming should use live depth inference inside the
sidecar.

The live path is intentionally narrow:

- D3D11 input only.
- D3D11VA decoded texture only.
- OpenVINO NPU only.
- Required model:
  `C:\Apps\arctrl\production\app\src\main\assets\distill_any_depth_small_588x336_nncf_int8.onnx`
- Required model shape:
  `FLOAT 1x3x336x588 -> FLOAT 1x1x336x588`
- No CPU/FP32/old-size model fallback.
- No static depth-file fallback.
- Inference or depth upload failure should fail the sidecar instead of silently
  falling back to flat output.

The `588x336` input size matches iw3's keep-aspect lower-bound `336` result for
16:9 content and is aligned to the Distill/Depth Anything 14-pixel patch grid.
Do not switch back to the older `518x518` square input or `700x392` unless a new
benchmark justifies it.

## Stereo Rendering

Stereo rendering lives in the HLSL string inside:

```text
app/acer_sr/moonlight_acer_sr.cpp
```

The shader:

- Converts NV12 to RGB on GPU.
- Treats the output texture as half-SBS:
  - left half = left eye
  - right half = right eye
- Samples the `R16_UNORM` depth texture.
- Computes a horizontal depth-based shift per pixel.
- Uses opposite sampling directions for left and right eye.
- In weak mode, uses a desktop2stereo-like simple shift formula.
- In medium/strong modes, uses depth-layered foreground/midground/background
  shift, subject lock, edge shift suppression, and background-aware repair.

Acer SR weaver consumes the generated half-SBS input texture through:

```text
weaver_->setInputViewTexture(...)
weaver_->weave()
swap_chain_->Present(1, 0)
```

The swapchain presents with vsync. Do not reintroduce tearing flags unless the
display path is deliberately redesigned.

## Build

Full Moonlight build uses the modified batch script:

```powershell
cd C:\Apps\arctrl\moonlight-qt
scripts\build-arch.bat release x64
```

The Acer SR sidecar can be rebuilt directly:

```powershell
cmake --build C:\Apps\arctrl\moonlight-qt\build\build-acer-sr-x64-release --config Release
```

After rebuilding the sidecar manually, copy these files into the deploy folder:

```powershell
Copy-Item C:\Apps\arctrl\moonlight-qt\build\build-acer-sr-x64-release\Release\moonlight-acer-sr.dll C:\Apps\arctrl\moonlight-qt\build\deploy-x64-release -Force
Copy-Item C:\Apps\arctrl\moonlight-qt\build\build-acer-sr-x64-release\Release\moonlight-acer-sr-smoke.exe C:\Apps\arctrl\moonlight-qt\build\deploy-x64-release -Force
```

The build expects local dependencies under `C:\Apps\build-tools`, including:

- Acer SpatialLabs / Simulated Reality SDK headers and runtime DLLs.
- `Intel.ML.OnnxRuntime.OpenVino.1.24.1` NuGet native runtime.

`scripts/build-arch.bat` copies Acer SR runtime DLLs and ONNX Runtime OpenVINO
DLLs into the deploy directory when their local paths are available.

## Run

Launch Moonlight with Acer SR enabled:

```powershell
cd C:\Apps\arctrl\moonlight-qt
.\scripts\run-acer-sr.ps1
```

Useful options:

```powershell
.\scripts\run-acer-sr.ps1 -Mode stereo -Strength weak
.\scripts\run-acer-sr.ps1 -Mode depth
.\scripts\run-acer-sr.ps1 -Strength medium
.\scripts\run-acer-sr.ps1 -Strength strong
```

The script sets:

```text
MOONLIGHT_ACER_SR=1
MOONLIGHT_ACER_SR_MODE=flat|stereo|depth
MOONLIGHT_ACER_SR_STRENGTH=weak|medium|strong
MOONLIGHT_ACER_SR_DEPTH_MODEL=<588x336 NNCF INT8 model>
```

Set this for verbose sidecar logs:

```powershell
$env:MOONLIGHT_ACER_SR_CONSOLE_LOG = "1"
```

## Test

Run a live-depth sidecar smoke test without Sunshine:

```powershell
cd C:\Apps\arctrl\moonlight-qt
.\scripts\run-acer-sr-smoke.ps1 -Frames 60 -Fps 60 -Mode stereo -Strength weak
```

The default smoke path does not inject synthetic depth. It exercises:

```text
synthetic NV12 frame -> live NPU depth -> D3D11 stereo shader -> Acer SR weaver
```

Expected current behavior on the verified machine:

- `Depth model IO inputType=1 outputType=1 shape=588x336`
- `Depth NPU frame` stabilizes around 40-41 ms in the sidecar.
- First run for a new model shape may spend many seconds compiling/caching the
  OpenVINO graph; later starts are much faster.

The separate OpenVINO Python benchmark for the same model has measured roughly:

```text
avg=28.90ms min=27.81ms p50=28.34ms
```

That benchmark excludes D3D11 readback, preprocessing, ORT wrapper overhead,
shader rendering, SR weaver, and present.

## Model Assets

Model generation scripts live in the parent Arctrl workspace:

```text
C:\Apps\arctrl\scripts\export-distill-any-depth-small.py
C:\Apps\arctrl\scripts\quantize-distill-any-depth-nncf.py
```

The model files live in:

```text
C:\Apps\arctrl\production\app\src\main\assets
```

They are intentionally ignored by the parent repository. Do not commit ONNX,
XML, or BIN model artifacts.

## Git Notes

This `moonlight-qt` directory is its own git checkout inside the Arctrl
workspace. The parent Arctrl repository currently sees it as an untracked
directory. Use the correct working directory when checking status or preparing
commits.

Do not commit build outputs from:

```text
build/
build-acer-sr/
```

Do not commit local SDK/runtime DLLs, OpenVINO runtime files, generated models,
or deployment artifacts.
