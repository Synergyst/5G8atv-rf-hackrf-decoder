# 5G8atv-rf-hackrf-decoder (fpvdec)

[日本語 README はこちら](README.ja.md)

A software receiver for **5.8 GHz analog FPV video (FM-ATV, NTSC)** using a
HackRF One — real-time color decoding of drone VTX signals on your PC.
C++20 + libhackrf + SDL2 + Dear ImGui, no GNU Radio required.

Forked from
[GOROman/famicom-rf-hackrf-decoder](https://github.com/GOROman/famicom-rf-hackrf-decoder)
(a Famicom VHF RF decoder); the NTSC decoder core is inherited, the RF
front-end was rebuilt for FM-ATV.

Demo video (click to watch on YouTube):

[![Demo video](https://img.youtube.com/vi/dDNk-uRtcGw/maxresdefault.jpg)](https://www.youtube.com/watch?v=dDNk-uRtcGw)

Live decode of a real 25 mW whoop VTX — HackRF One on the left, fpvdec
running on the laptop (Betaflight OSD visible):

![Setup: HackRF One + whoop + fpvdec live decode](docs/IMG_9715.jpeg)

## Supported channels

All 40 standard 5.8 GHz analog FPV channels, selected as band letter +
channel number (e.g. `--channel F4`):

| Band | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 |
|---|---|---|---|---|---|---|---|---|
| **A** (Boscam) | 5865 | 5845 | 5825 | 5805 | 5785 | 5765 | 5745 | 5725 |
| **B** (Boscam) | 5733 | 5752 | 5771 | 5790 | 5809 | 5828 | 5847 | 5866 |
| **E** | 5705 | 5685 | 5665 | 5645 | 5885 | 5905 | 5925 | 5945 |
| **F** (Airwave) | 5740 | 5760 | 5780 | 5800 | 5820 | 5840 | 5860 | 5880 |
| **R** (Raceband) | 5658 | 5695 | 5732 | 5769 | 5806 | 5843 | 5880 | 5917 |

A two-stage **AFC** tracks the VTX automatically: a coarse acquisition
stage pulls in transmitters that start 2–3 MHz off after a cold power-on,
and a fine stage keeps the receiver centered as the VTX drifts while
warming up (real units drift ~1 MHz in the first minutes).

## Interface

fpvdec ships with **two graphical interfaces** selectable at startup:

### ImGui Mode (default)

A modern, resizable window with a floating **Control Panel** in the top-left
corner. The panel is organized into toggleable sections (checkboxes at the
bottom):

| Section | Shows |
|---|---|
| **Channel Info** | Nearest FPV channel name + tuned frequency in MHz |
| **Sync Status** | Line / V-SYNC state with color coding (green=OK, orange=lost line, red=searching) |
| **Signal Bars** | Ring buffer fill level + chroma/burst amplitude (color strength) |
| **AGC Info** | Gain mode (auto/manual), LNA & VGA levels, RF amp state |
| **Stats** | Frame count, dropped bytes, clipping flag, video latency |

No keyboard hotkeys are active in this mode.

### SDL Mode (`--gui sdl`)

Restores the original behavior with legacy keyboard hotkeys:

| Key | Action |
|---|---|
| `q` / `ESC` | Quit |
| `a` | Toggle gain auto/manual |
| `l` / `L` | LNA up / down |
| `g` / `G` | VGA up / down |
| `b` | Toggle RF amp (+14 dB) |
| `c` | Toggle color/gray |
| `o` | Toggle OSD on/off |
| `s` | Screenshot |
| `h` | Toggle help overlay |
| `←` / `→` | Tune ±50 kHz |
| `↑` / `↓` | Tune ±1 MHz |
| `r` | Toggle CRT emulation |
| `v` | Record/stop IQ |

The SDL overlay renders a big green channel readout (top-left), a yellow
status line across the top, and a red `CLIP` indicator on ADC clipping.

## Hardware

[HackRF One](https://greatscottgadgets.com/hackrf/one/) receive-only at
10 MSPS. The stock whip antenna works at desk range; for real distance use
a **5.8 GHz circular-polarized patch/helical antenna** — FPV VTX are
circular-polarized, so a linear whip loses 3 dB plus deep multipath fades.
The +14 dB RF amp is enabled by default (`--no-amp` to disable).

## Build

### Windows (MSVC)

Requires Visual Studio 2022, CMake, [PothosSDR](https://downloads.myriadrf.org/builds/PothosSDR/)
(for libhackrf) and the SDL2 VC development package extracted to
`third_party/SDL2` (with the headers copied into `include/SDL2/`).

```sh
cmake -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

`SDL2.dll` / `hackrf.dll` are copied next to the exe automatically.

### macOS / Linux

```sh
brew install hackrf sdl2 cmake pkg-config   # or apt equivalents
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

## Usage

```sh
# Live: tune a channel preset (AFC + auto gain take it from there)
./build/fpvdec --channel F4

# Explicit frequency / inverted-polarity VTX
./build/fpvdec --freq 5806e6 --invert

# Record raw IQ while decoding, replay later
./build/fpvdec --channel R1 --record cap.cs8
./build/fpvdec --input file --file cap.cs8 --loop

# Headless: dump decoded frames as PPM (debug / verification)
./build/fpvdec --input file --file cap.cs8 --dump-frames out_ --frames 30

# Legacy SDL mode with hotkeys (for testing)
./build/fpvdec --channel F4 --gui sdl
```

### Options

| Option | Description |
|---|---|
| `--channel NAME` | FPV channel preset, band A/B/E/F/R + 1-8 (default F4) |
| `--freq HZ` | explicit carrier frequency |
| `--dev HZ` | FM peak deviation (default 5e6) |
| `--invert` | flip discriminator polarity (non-standard VTX) |
| `--no-afc` | disable automatic frequency centering |
| `--gain auto\|manual` | RF gain control (default auto) |
| `--lna N` / `--vga N` | fixed gains, imply `--gain manual` |
| `--no-amp` | disable the +14 dB RF preamp (default on) |
| `--rate HZ` | sample rate (default 10e6; 8e6 keeps coarse color, ≤7e6 grayscale, ~6e6 minimum) |
| `--lpf HZ` | optional post-detector video LPF, e.g. 4.2e6 |
| `--mode color\|gray` | decode mode (default color) |
| `--sat F` / `--hue DEG` | color trims |
| `--overscan F` | horizontal crop per side (default 0 — FPV OSD sits at the edges) |
| `--record PATH` | tee raw IQ to .cs8 while decoding |
| `--dump-frames PREFIX` / `--frames N` | headless PPM frame dump |
| `--dump-composite PATH` | dump post-AGC composite as f32 (debug) |
| `--spectrum` | print PSD and exit |
| `--gui imgui\|sdl` | GUI mode: **imgui** (default, floating control panel, no hotkeys) or **sdl** (legacy hotkeys for testing) |

## How it works

```
HackRF One (10 MSPS, tuned to the channel; AFC re-centers on the VTX)
  → complex DC blocker
  → 4.9 MHz complex channel LPF (flat through the chroma upper sideband)
  → FM quadrature discriminator (AVX2; sync-tip-positive polarity)
  → AGC (per-line sync-tip / back-porch tracking = pedestal clamp)
  → sync separation on a dedicated 1 MHz low-pass stream (~7 dB noise margin)
  → flywheel line PLL with interlace-aware half-line re-anchoring
  → 3.58 MHz chroma BPF, per-line burst measurement, U/V demod
  → YUV→RGB 640×480 (fields line-doubled, ~59.94 updates/s)
  → triple buffer → GUI display (ImGui or SDL)
```

Compared to the Famicom original: FM instead of AM detection, two-stage
AFC (mean-frequency acquisition + occupied-band-midpoint tracking with
feed-forward level compensation into the AGC), automatic RF gain from ADC
peak statistics, a narrowband sync path for weak signals, and real-camera
interlace handling. The DSP hot paths (FIR kernels, discriminator,
atan2) are AVX2-vectorized; the full chain runs ~2.4× real time at
10 MSPS on an i7-9700K.

The GUI layer is split into two independent modules:
- **`gui_manager`** — Dear ImGui frontend (Control Panel, signal bars, toggles)
- **`sdl_display`** — legacy SDL2 renderer (pixel-level OSD, CRT emulation, bitmap fonts)

These are mutually exclusive. Adding an ImGui control only touches `gui_manager`;
modifying the SDL overlay only touches `sdl_display`.

## Tests

```sh
./build/synth_fm            # FM color bars → decode → assert RGB
./build/synth_fm bars.cs8   # write synthetic IQ for E2E tests
ctest --test-dir build -C Release
```

## Troubleshooting

| Symptom | Fix |
|---|---|
| Snow, never syncs | check the VTX channel; watch the console for `AFC (coarse)` pulling in; try `--spectrum` |
| Black-and-white picture that looks inverted | non-standard VTX polarity → `--invert` |
| Weak / sparkly at range | use a circular-polarized 5.8 GHz antenna; keep the RF amp on |
| Distorted up close | auto gain backs off, but very close in also toggle the amp off (`b`) |
| No color | signal too weak for burst detection, or `--rate` below 8 MSPS |
| Video latency climbing | CPU can't keep up — lower `--rate` (8e6 / 6e6) |

## License / disclaimer

MIT License — see [LICENSE](LICENSE). Based on
[famicom-rf-hackrf-decoder](https://github.com/GOROman/famicom-rf-hackrf-decoder)
by GOROman (MIT).

Receive-only tool. It never transmits with the HackRF One. Receiving FPV
video may require an amateur radio license and/or VTX registration in
your country (it does in Japan) — fly and receive legally.
