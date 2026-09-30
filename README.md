# Vixen

Experimental Linux proof-of-concept for running NVIDIA Maxine Video Effects
through Wine.

Vixen currently provides a small Windows worker that accepts raw BGR24 video
frames through stdin, processes them using NVIDIA Maxine Artifact Reduction and
Super Resolution, and writes the resulting frames to stdout.

This makes it possible to build a Linux video pipeline such as:

```text
FFmpeg
  │ BGR24
  ▼
vixen.exe (Wine)
  │
  ├─ NVIDIA Maxine VFX
  ├─ Artifact Reduction
  └─ Super Resolution
  │ BGR24
  ▼
FFmpeg
```

The project is currently a proof of concept. Setup is extremely manual.

## Status

Working:

- NVIDIA Maxine VFX under Wine
- Artifact Reduction
- Super Resolution
- Persistent multi-frame processing
- Raw BGR24 stdin/stdout streaming
- 720p -> 1080p video
- 1080p -> 4K video
- Faster-than-realtime 1080p -> 4K processing has been observed on an
  RTX 4070 SUPER

Not implemented yet:

- User-friendly Linux frontend
- Automatic input resolution / framerate detection
- Audio and subtitle handling
- mpv integration
- Automatic Wine prefix setup
- Automatic dependency installation
- Zero-copy GPU frame transport

## How it works

Maxine VFX does not provide a native Linux runtime.

Vixen does not attempt to port Maxine to Linux. Instead, `vixen.exe` is
cross-compiled as a Windows executable using MinGW and runs inside Wine.

The Windows Maxine runtime then communicates with the Linux NVIDIA stack using
Wine compatibility libraries.

Yes, this is slightly ridiculous.

Interestingly, NVIDIA's own proxy source contains:

```cpp
#if defined(linux) || defined(unix) || defined(__linux)
#warning nvVideoEffectsProxy.cpp not ported
```

Vixen therefore does not port it. It compiles it for Windows. On Linux.

## Requirements

The currently tested setup uses:

- Linux
- NVIDIA GPU
- NVIDIA proprietary driver
- Wine
- MinGW-w64
- FFmpeg / ffprobe
- CMake
- NVIDIA Maxine VFX runtime and models
- `nvidia-libs`
- DXVK
- DXVK-NVAPI

The initial development system uses an NVIDIA GeForce RTX 4070 SUPER.

Other configurations have not yet been tested.

## NVIDIA source files

Vixen vendors the small subset of NVIDIA's Maxine VFX SDK source required to
compile the worker:

```text
third_party/nvvfx/
├── include/
│   ├── nvCVImage.h
│   ├── nvCVStatus.h
│   ├── nvTransferD3D.h
│   ├── nvTransferD3D11.h
│   └── nvVideoEffects.h
└── src/
    ├── nvCVImageProxy.cpp
    └── NVVideoEffectsProxy.cpp
```

The NVIDIA Maxine runtime DLLs and models are **not** included in this
repository.

See the NVIDIA license notices in `third_party/nvvfx`.

## Wine prefix

Create or choose a dedicated Wine prefix:

```bash
export WINEPREFIX="$HOME/.local/share/wineprefixes/maxine"
wineboot -u
```

The currently working development prefix is:

```text
~/.local/share/wineprefixes/maxine
```

Install the NVIDIA Maxine VFX runtime into this prefix.

Vixen has currently been tested with the Maxine VFX 0.7.6 runtime/models.

## NVIDIA driver compatibility libraries

Vixen currently relies on `nvidia-libs`:

https://github.com/SveSop/nvidia-libs

Build/download the libraries according to that project's instructions and
install them into the Vixen Wine prefix.

For example:

```bash
./setup_nvlibs.sh install
```

After installation, the prefix should contain NVIDIA compatibility DLLs such
as:

```text
drive_c/windows/system32/nvcuda.dll
drive_c/windows/system32/nvapi64.dll
```

These may be symlinks into the `nvidia-libs` checkout. Do not remove that
checkout while those links are in use.

`nvidia-libs` reports NVML as optional and disabled by default. Vixen does not
currently require NVML.

### Verify CUDA

Before debugging Vixen itself, verify that CUDA through Wine can see the NVIDIA
GPU.

A working configuration should identify the real GPU, for example:

```text
NVIDIA GeForce RTX 4070 SUPER
SM version: 8.9
```

If CUDA cannot see the GPU, Vixen will not work.

## DXVK and DXVK-NVAPI

The Wine prefix also needs a working DXVK/DXVK-NVAPI setup.

During development, DXVK's `d3d11.dll` and `dxgi.dll` were installed as native
DLL overrides in the prefix.

DXVK-NVAPI must be able to initialize against the NVIDIA GPU.

A useful sanity check is the DXVK-NVAPI test suite. The working development
configuration reports:

```text
All tests passed (104 assertions in 1 test case)
```

### GPU selection

On hybrid-GPU systems, make sure DXVK selects the NVIDIA GPU rather than the
integrated GPU.

The development machine contains both:

```text
NVIDIA GeForce RTX 4070 SUPER
Intel UHD Graphics 770
```

The following environment variable is used to select NVIDIA explicitly:

```bash
export DXVK_FILTER_DEVICE_NAME="NVIDIA GeForce RTX 4070 SUPER"
```

The DXVK configuration used during development also contains:

```ini
dxgi.hideNvidiaGpu = False
```

For example:

```bash
cat >/tmp/maxine-dxvk.conf <<'EOF'
dxgi.hideNvidiaGpu = False
EOF

export DXVK_CONFIG_FILE=/tmp/maxine-dxvk.conf
```

If this is wrong, Maxine may fail with:

```text
Error: The currently installed graphics driver is not supported
```

even though the actual NVIDIA driver is perfectly valid.

### DLSS Vulkan layer

During development the NVIDIA DLSS Vulkan layer was present on the system.
For Vixen testing it was disabled with:

```bash
export DLSSNR_DISABLE=1
```

This avoids unrelated DLSS layer behaviour interfering with diagnosis.

## Building

Vixen's worker is a **Windows executable**.

It must therefore be cross-compiled using MinGW rather than built as a native
Linux ELF executable.

Configure the MinGW build:

```bash
cmake -S . -B build-mingw \
  -DCMAKE_SYSTEM_NAME=Windows \
  -DCMAKE_C_COMPILER=x86_64-w64-mingw32-gcc \
  -DCMAKE_CXX_COMPILER=x86_64-w64-mingw32-g++
```

Build:

```bash
cmake --build build-mingw -j"$(nproc)"
```

The resulting executable should be:

```text
build-mingw/vixen.exe
```

Depending on how the executable is linked, MinGW runtime DLLs may also be
required by Wine, including:

```text
libgcc_s_seh-1.dll
libstdc++-6.dll
libwinpthread-1.dll
```

TODO: make the release build static or otherwise package these cleanly.

## Worker interface

The current worker operates on raw BGR24 frames.

Conceptually:

```text
vixen.exe INPUT_WIDTH INPUT_HEIGHT OUTPUT_HEIGHT
```

For example:

```bash
wine build-mingw/vixen.exe 1280 720 1080
```

means:

```text
input:  1280x720 BGR24
output: 1920x1080 BGR24
```

The aspect ratio is preserved when calculating the output width.

### Important: rawvideo contains no metadata

The dimensions supplied to Vixen **must exactly match the dimensions emitted
by FFmpeg**.

For BGR24, one input frame contains:

```text
width * height * 3
```

bytes.

If FFmpeg emits 1920x1080 while Vixen is told the input is 1280x720, Vixen
cannot detect the mistake. It will interpret arbitrary chunks of adjacent
frames as individual images.

The resulting video is impressively cursed.

## Example: 720p -> 1080p

```bash
ffmpeg -v error \
  -i input.mkv \
  -an \
  -vf scale=1280:720 \
  -f rawvideo \
  -pix_fmt bgr24 \
  - \
| WINEDEBUG=-all wine build-mingw/vixen.exe 1280 720 1080 \
| ffmpeg -y \
  -f rawvideo \
  -pix_fmt bgr24 \
  -video_size 1920x1080 \
  -framerate 25 \
  -i - \
  -c:v libx264 \
  -crf 18 \
  -pix_fmt yuv420p \
  output.mp4
```

The `1280x720` passed to Vixen must match the output of the first FFmpeg
process.

Likewise, `1920x1080` passed to the second FFmpeg process must match Vixen's
output dimensions.

## Example: 1080p -> 4K

For a 1920x1080 source:

```bash
ffmpeg -v error \
  -i input.mkv \
  -an \
  -f rawvideo \
  -pix_fmt bgr24 \
  - \
| WINEDEBUG=-all wine build-mingw/vixen.exe 1920 1080 2160 \
| ffmpeg -y \
  -f rawvideo \
  -pix_fmt bgr24 \
  -video_size 3840x2160 \
  -framerate 25 \
  -i - \
  -c:v libx264 \
  -crf 18 \
  -pix_fmt yuv420p \
  output.mp4
```

A 60-second 1080p source has been processed to 4K in approximately 50 seconds
on the development RTX 4070 SUPER system.

This number should currently be treated only as an early proof-of-concept
measurement, not a benchmark.

## Troubleshooting

### Maxine says the graphics driver is unsupported

If you see:

```text
Error creating effects "ArtifactReduction & Upscale"
Error: The currently installed graphics driver is not supported
```

check which GPU DXVK-NVAPI has selected.

Vixen development initially encountered the Intel UHD Graphics 770 being
selected instead of the RTX 4070 SUPER.

Verify:

```bash
export DXVK_FILTER_DEVICE_NAME="NVIDIA GeForce RTX 4070 SUPER"
```

and:

```ini
dxgi.hideNvidiaGpu = False
```

### `nvcuda.dll` cannot be loaded

Check that `nvidia-libs` has been installed into the same Wine prefix used to
run Vixen:

```bash
find "$WINEPREFIX/drive_c/windows" \
  \( -iname 'nvcuda.dll' -o -iname 'nvapi64.dll' \) \
  -ls
```

### `libgcc_s_seh-1.dll`, `libstdc++-6.dll`, or `libwinpthread-1.dll` missing

These are MinGW runtime dependencies of the current worker build.

Install/copy the appropriate MinGW runtime DLLs or build Vixen with suitable
static runtime linkage.

### Video contains horizontal stripes / repeated image fragments

Check the raw input dimensions first.

Rawvideo carries no resolution metadata. If FFmpeg emits 1920x1080 frames but
Vixen is launched as:

```bash
vixen.exe 1280 720 1080
```

the byte stream will be split at the wrong boundaries.

This was, regrettably, discovered experimentally.

## Roadmap

The immediate goal is to replace the manual pipeline with something like:

```bash
vixen input.mkv -o output.mkv --height 2160
```

A native Linux frontend can then:

1. inspect the source with `ffprobe`;
2. configure decoding automatically;
3. start the Wine worker;
4. encode the processed frames;
5. preserve audio and subtitles.

Longer term, realtime integration with players such as mpv is of particular
interest.

## Disclaimer

Vixen is an experimental compatibility project and is not affiliated with or
endorsed by NVIDIA.

NVIDIA Maxine is NVIDIA software and requires its separately distributed
runtime/models and a compatible NVIDIA GPU/driver.

Vixen does not redistribute the proprietary Maxine runtime.

Vixen vendors portions of the NVIDIA Maxine VFX SDK interface and proxy sources. See third_party/nvvfx/LICENSE and the original source-file notices.
