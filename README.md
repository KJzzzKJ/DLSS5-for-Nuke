# DLSS 5 for Foundry Nuke

[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)
[![Status](https://img.shields.io/badge/Status-Experimental-F59E0B.svg)](https://github.com/KJzzzKJ/DLSS5-for-Nuke/releases)
[![Platform](https://img.shields.io/badge/Platform-Windows%2010%20%2F%2011%20(x64)-0078D6.svg)](https://www.microsoft.com/windows)
[![Nuke Versions](https://img.shields.io/badge/Nuke-15.0%20%7C%2017.0-F9A01B.svg)](https://www.foundry.com/products/nuke)
[![Hardware](https://img.shields.io/badge/GPU-NVIDIA%20RTX-76B900.svg)](https://www.nvidia.com)

Experimental, unofficial native plug-in for using DLSS Neural Rendering in
Foundry Nuke. `DLSS5Live` supports still images and temporal sequences through
a native Nuke node and an isolated background worker process.

> **Experimental warning**
>
> This project uses observed, undocumented runtime behavior. It is not
> affiliated with, endorsed by, or supported by NVIDIA or Foundry. Compatibility
> can change with Nuke, GPU driver, and user-supplied runtime versions.

## What it does

`DLSS5Live` processes a Nuke image stream with neural reconstruction controls,
optional temporal history, and optional guide inputs. It is intended for
Windows systems with an NVIDIA RTX GPU.

Under the hood, `DLSS5Live` integrates a two-pass neural execution pipeline:
1. **Pass 1 (DLSS-NR / Feature 18)**: Evaluates neural reconstruction at native
   `1.0x` resolution into an intermediate linear `RGBA16F` texture, applying
   neural tuning and style controls.
2. **Pass 2 (DLSS-SR / Feature 1)**: When upscaling (> `1.0x`) is requested,
   evaluates AI Super Resolution on Tensor Cores to upscale the intermediate
   texture to the target resolution. When `1.0x (DLAA)` is chosen, this pass is
   bypassed for zero overhead.

The node supports single images, temporal sequences, and experimental CG
multi-pass workflows.

## Requirements

| Component | Requirement |
| --- | --- |
| Operating system | Windows 10 or Windows 11, 64-bit |
| GPU | NVIDIA RTX GPU |
| Nuke | Nuke 15.0 or Nuke 17.0 build matching the supplied plug-in DLL |
| Runtime | A compatible runtime obtained and configured legally by the user |

Nuke point releases require their own compatible plug-in build. Do not assume a
DLL built for Nuke 15.0 is automatically compatible with Nuke 15.1 or 15.2.

## Installation

1. Download the intended Release ZIP from the repository's
   [Releases](https://github.com/KJzzzKJ/DLSS5-for-Nuke/releases) page.
2. Extract it to a local folder.
3. Run `install.bat`.
4. Restart Nuke, then press **Tab** and create `DLSS5Live`.
5. In the node properties, set **nvngx.dll (Worker) Path** to your compatible
   local runtime/worker location (or define the `NUKE_DLSS5_WORKER_PATH`
   environment variable).

The installer creates an isolated folder under `~/.nuke/DLSS5Live/`, copies the
plug-in files, registers the plug-in path, and adds the `DLSS5` node menu.

## Runtime policy

The public v1.0 release policy is deliberately narrow:

- The Release ZIP includes project-authored installation files and project-built
  Nuke plug-in DLLs.
- It does **not** include or redistribute NVIDIA, NGX, DLSS, ReShade, RenoDX,
  or other third-party runtime binaries.
- Users must obtain any required runtime from a source and under terms that
  permit their use. Do not attach proprietary runtime DLLs to issues or pull
  requests.

The repository's MIT license applies only to project-authored code and does not
relicense third-party software, SDKs, models, or trademarks.

## Using DLSS5Live

### Inputs

| Input | Label | Use |
| --- | --- | --- |
| 0 | `Source` | Required source image stream. |
| 1 | `motion` | Optional external motion-vector stream when External Input 1 is selected. |
| 2 | `depth` | Optional depth guide for CG Multi-pass (`depth.Z`, `red`, or `alpha`). |
| 3 | `mask` | Optional control-mask guide for CG Multi-pass (`alpha` or `red`), dynamically bound to `DLSSNR.ControlMask` (e.g. Roto or object mask). |

### Pipeline modes

- **Single Frame (Default)** evaluates each frame independently and resets
  temporal history every frame. This is the recommended default for stills,
  timeline scrubbing, and matte painting work to prevent temporal ghosting.
- **Sequence** keeps temporal history across consecutive forward frames. Use a
  consistent frame order and reset history when the sequence changes.
- **CG Multi-pass** exposes the depth and control-mask inputs in addition to
  source and motion. The interface is present, but its output difference has
  not yet been fully validated; use it as an experimental workflow.

### Motion Vector Source

In **Sequence** and **CG Multi-pass**, choose one of these sources:

| Source | Behavior | When to use it |
| --- | --- | --- |
| **Auto Flow (OpenCV DIS)** | Calculates dense optical flow from consecutive Source frames in the background. | A normal image sequence without a separate motion-vector pass. |
| **External Input 1** | Reads motion vectors from input 1. | A compositing or CG workflow that already provides a suitable motion-vector pass. |
| **None / Zero Motion** | Sends zero motion and does not use external vectors. | Stills, a deliberate no-motion test, or content where no usable vectors exist. |

#### OpenCV DIS Presets

When **Auto Flow (OpenCV DIS)** is active:

| Preset | Resolution | Speed | Intended Use |
| --- | --- | --- | --- |
| **Fast Preview** | 480p | ~2 ms | 24 fps interactive viewer playback. |
| **Balanced (Default)** | 640p | ~6 ms | Standard production balance between accuracy and performance. |
| **High Quality** | 960p | ~15 ms | High-frequency detail and complex foreground motion. |
| **Extreme** | 1280p | ~40 ms | Offline final rendering. |
| **Custom** | User-defined | Varies | Manually specify **Flow Width** and **Iterations**. |

#### External Motion Vector Knobs

When **External Input 1** is active:
- **MV Channels**: Selects 2 channels for U (X) and V (Y) displacement (defaults to `forward`, `motion`, or RG).
- **MV Scale X / Y**: Multiplier for vector magnitude.
- **Invert X / Y**: Inverts horizontal or vertical vector directions.

## Node Controls

### DLSS Resolution & Model

- **Upscaling Mode**:
  - `1.0x (DLAA / Native)`: Pure Neural Reconstruction at native resolution.
  - `1.5x (Quality)`, `1.72x (Balanced)`, `2.0x (Performance)`, `3.0x (Ultra Performance)`:
    Two-pass pipeline (DLSS-NR + DLSS-SR).
- **DLSS Model Preset**: Model architecture presets (`Default`, `J`, `K`, `L`, `M`).
  Preset `J` is optimized for neural reconstruction.
- **NR Style**: Neural reconstruction style profile:
  - `Default`: Balanced neural enhancement.
  - `Natural`: Organic, soft detail retention.
  - `Cinematic`: Film-grade texture and grain preservation.
- **NR Preset**: Tuning profile (`Default`, `Preset #1`, `Preset #2`, `Preset #3`).
- **Automatic Mask**: Automatically masks out non-neural UI or static elements.

### Neural Tuning

- **Intensity** (`0.0` - `2.0`, default `1.0`): Global Neural Reconstruction strength.
- **Local Tone** (`0.0` - `2.0`, default `1.0`): Local tone mapping and dynamic contrast adjustment.
- **Local Structure** (`0.0` - `2.0`, default `1.0`): Local structural detail and texture enhancement.
- **Skin Structure** (`-1.0` - `2.0`, default `-1.0`): Specialized skin and facial detail enhancement.
  The default `-1.0` automatically inherits the value of **Local Structure**. Values from `0.0` to `2.0`
  override and independently adjust skin fidelity.

### Color & Dynamic Range

- **Color Bit Depth**:
  - `16-bit Half Float (Scene-Linear, Recommended)` (Default): Preserves full scene-linear
    dynamic range through the IPC transport.
  - `8-bit Integer (SDR Legacy)`: Legacy compatibility path where RGB is clamped to `0-1`
    and quantized to 8-bit in both directions.
- **Enable HDR Range**: Checkbox to activate highlight dynamic range compression.
- **HDR Range Scale** (`1.0` - `64.0`, default `2.0`):
  When enabled, divides RGB by this ratio before DLSS-NR (Feature 18) and restores (multiplies)
  it afterward. For example, `2.0` maps an input value of `2.0` to `1.0` for the neural model
  and restores it to `2.0` upon return. This protects bright lights, sun, and fire highlights
  from neural clamping. Alpha, motion vectors, depth, and mask guides are not scaled.

### Scanline Layout Alignment

Nuke natively indexes image scanlines from the bottom up (row `y = 0` at the bottom).
DirectX 12 and the worker operate with top-down textures (row `y = 0` at the top).
`DLSS5Live` automatically converts scanline coordinates for the main image, motion
vectors, depth, and control masks before transfer, and aligns the worker output back
into Nuke's row cache, eliminating vertical flip and orientation issues.

## Build from source

Building requires Visual Studio 2022 with Desktop development with C++, CMake,
Ninja, and a local Nuke NDK installation matching the target DLL. Build the
Nuke plug-in and worker with the repository scripts, then package only the
publicly approved files for a Release.

```powershell
powershell.exe -ExecutionPolicy Bypass -File tools/build_multi.ps1
powershell.exe -ExecutionPolicy Bypass -File worker/build.ps1
```

### Build for an unlisted Nuke version

The repository includes the C++ source, CMake files, and build scripts needed
to compile `DLSS5Live.dll` against a local Nuke installation. If your Nuke
version is not listed in a Release, build the plug-in against that version's
own NDK instead of renaming an existing DLL.

For example, a Nuke 15.2 installation can be supplied to the Nuke 15 build
argument:

```powershell
powershell.exe -ExecutionPolicy Bypass -File tools/build_multi.ps1 `
    -Nuke15Dir "C:\Program Files\Nuke15.2v1"
```

The result is written to `bin/Nuke15/DLSS5Live.dll`. Back up any existing
local DLL, then copy this result to the corresponding `bin/Nuke15/` folder
under your `~/.nuke/DLSS5Live/` installation. Use `-Nuke17Dir` in the same way
for a Nuke 17 installation.

Building the project-owned DLL does not provide NVIDIA or other third-party
runtime files. You remain responsible for supplying a compatible runtime under
its applicable terms. A successful compilation is not proof that an unlisted
Nuke version or runtime combination has been tested by this project.

Packaging and uploading a Release are maintainer-controlled actions. The public
CI checks source code; it does not publish Releases or bundle third-party
runtimes automatically.

## License and disclaimer

Project-authored source is licensed under [MIT](LICENSE). NVIDIA, DLSS, NGX,
Foundry, and Nuke are trademarks or registered trademarks of their respective
owners. This project does not claim ownership of their SDKs, runtimes, or
trademarks.
