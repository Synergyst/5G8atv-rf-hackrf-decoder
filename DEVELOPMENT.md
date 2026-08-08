# fpvdec Development Reference Manual

A quick-reference guide for understanding the codebase structure, conventions,
and common pitfalls. Written from accumulated experience modifying this code.

---

## 1. Directory Layout

```
src/
├── main.cpp              ← CLI parsing, thread orchestration, render loop
├── config.hpp            ← All configuration fields (single source of truth)
├── dsp/
│   ├── frame.hpp         ← Frame struct + TripleBuffer (output container)
│   ├── ntsc_decoder.cpp  ← DSP core: sync detection, chroma demod, row rendering
│   ├── ntsc_decoder.hpp  ← Public API + Stats struct
│   ├── chroma.hpp        ← Burst measurement helper
│   ├── agc.hpp           ← Automatic gain control
│   ├── fir.hpp           ← FIR filter utilities
│   ├── fm_detector.hpp   ← Quadrature discriminator
│   ├── dc_blocker.hpp    ← DC offset removal
│   ├── nco.hpp           ← Numerically controlled oscillator
│   └── sync.hpp          ← Sync separator + PLL
├── source/
│   ├── sample_source.hpp ← Abstract input interface
│   ├── hackrf_source.cpp ← HackRF One input
│   └── file_source.cpp   ← .cs8 recording replay
├── ui/
│   ├── sdl_display.cpp   ← SDL2 window/renderer, CRT LUT, OSD text
│   ├── sdl_display.hpp   ← SdlDisplay class + CrtLut + OsdStats
│   ├── gui_manager.cpp   ← ImGui overlay (Control Panel)
│   ├── gui_manager.hpp   ← GuiManager class
│   └── imgui_display.hpp ← ImGui helper utilities
└── util/
    ├── fpv_channels.hpp  ← 5.8 GHz channel presets
    └── spectrum.hpp      ← PSD utilities

tests/
└── synth_fm.cpp          ← E2E test: synthetic IQ → decode → assert RGB

third_party/              ← External dependencies (SDL2, etc.)
```

### Key file relationships:

```
main.cpp
  ├── Config (config.hpp)
  ├── ISampleSource → HackRfSource / FileSource
  ├── NtscDecoder (dsp/ntsc_decoder.cpp)
  ├── TripleBuffer (dsp/frame.hpp)
  ├── SdlDisplay (ui/sdl_display.cpp)
  └── GuiManager (ui/gui_manager.cpp)
```

---

## 2. Architecture Overview

### Data flow (DSP thread):

```
HackRF / File → complex IQ → DC blocker → channel LPF → FM discriminator
  → AGC → sync separator → flywheel PLL → chroma BPF → U/V demod → RGB Frame
  → publish to TripleBuffer
```

### Data flow (UI thread):

```
TripleBuffer.acquire() → Frame (RGBA) → SdlDisplay.render() or GuiManager.render()
  → SDL renderer → display
```

### Key design decisions:

- **TripleBuffer** — Lock-free three-slot frame buffer. DSP writes to `back()`,
  publishes via atomic swap. UI acquires latest via another atomic swap.
  Three slots prevent GPU wait stalls.

- **NtscDecoder** — Thread-safe single-producer. The decoder's `process()`
  method is called from the DSP thread with raw detected envelope samples.
  It manages its own internal ring buffers (`comp_`, `chromab_`, `syncb_`)
  and publishes completed frames to the TripleBuffer.

- **SdlDisplay** — Manages one SDL window, one renderer, one texture.
  Creates three internal `Frame` objects: `last_frame_`, `osd_frame_`,
  `crt_frame_`. The CRT LUT (`CrtLut`) is built once at init time from
  the window dimensions.

- **GuiManager** — Dear ImGui overlay rendered on top of SDL. Transparent
  windows, no chrome, positioned by `Config::overlay_position`.

---

## 3. Common Pitfalls

### 3.1. `Frame` dimensions are dynamic — never use `static constexpr`

`Frame` used to have `static constexpr int kWidth = 640` / `kHeight = 480`.
These are now instance members (`int width`, `int height`) set via `resize()`.

**Wrong:** `Frame::kWidth`, `Frame::kHeight`
**Right:** `f.width`, `f.height` (where `f` is a `Frame&`)

This change propagates to every file that references frame dimensions. Use
`grep -rn "Frame::kWidth\|Frame::kHeight"` to find any remaining references.

### 3.2. Anonymous namespace vs. header declarations

Functions defined in an anonymous namespace (`namespace { ... }`) in a `.cpp`
file have **internal linkage** — they are invisible to the linker and to other
translation units.

If you declare a function in a header (e.g., `void apply_crt(...);`) and
define it inside an anonymous namespace in the `.cpp`, the linker will fail
with an "undefined reference" error.

**Fix:** Move the definition outside the anonymous namespace, or declare the
function `static` in the `.cpp` and don't put it in the header.

### 3.3. Duplicate struct/class definitions

The `CrtLut` struct was accidentally defined in both `sdl_display.cpp`
(anonymous namespace) and `sdl_display.hpp` (famidec namespace), causing
"reference to 'CrtLut' is ambiguous" compiler errors.

**Rule:** Define structs/types in the header. Only define functions in the `.cpp`
(unless they're inline or template).

### 3.4. TripleBuffer resize must happen before NtscDecoder construction

The `TripleBuffer` holds `Frame` objects. If you change the resolution after
the decoder is constructed, the decoder will still write to the old-sized
buffers, causing buffer overflows.

**Correct order:**
```cpp
TripleBuffer tb;
tb.resize(width, height);  // Resize first
NtscDecoder dec(cfg, tb);  // Then construct decoder
```

### 3.5. CRT LUT rebuild

`CrtLut` is a barrel-distortion lookup table for CRT mode, built once from
the window dimensions. If the resolution changes, you must call
`disp.rebuild_crt_lut(width, height)` to rebuild it. The LUT is a member
of `SdlDisplay`, not a global singleton.

### 3.6. synth_fm.cpp breaks when Frame dimensions change

The E2E test `tests/synth_fm.cpp` directly accesses `Frame::kWidth` and
`Frame::kHeight`. If you change these, the test will fail to compile or
assert incorrectly. Keep this in mind when modifying `frame.hpp`.

---

## 4. Adding a New CLI Flag

1. **Document it** in `usage()` in `main.cpp`
2. **Add the field** to `Config` in `config.hpp` (with a sensible default)
3. **Parse it** in `parse_args()` in `main.cpp`
4. **Consume it** wherever it's needed (decoder, display, etc.)

Example:
```cpp
// config.hpp
bool feature_enabled = false;  // default: off

// main.cpp usage()
"  --enable-feature      turn on the feature\n"

// main.cpp parse_args()
else if (a == "--enable-feature") cfg->feature_enabled = true;
```

---

## 5. Adding a New Display Feature

1. **Modify `Frame`** if you need new data in the output frame.
2. **Update `NtscDecoder::process()`** to compute the new data.
3. **Update `SdlDisplay::render()`** to draw it.
4. **Update `GuiManager::render()`** if the feature should show in the overlay.

Changes to `SdlDisplay` and `GuiManager` are **mutually exclusive** in their
responsibilities: SDL handles the pixel-level rendering, ImGui handles the
floating control panel.

---

## 6. Build Commands

```bash
# Configure (once)
cmake -B build -DCMAKE_BUILD_TYPE=Release

# Build
cmake --build build -j$(nproc)

# Run tests
ctest --test-dir build -C Release

# Run fpvdec (headless demo with synthetic IQ)
./build/fpvdec --input file --file synth_qm.cs8 --dump-frames out_ --frames 10
```

### Common build errors:

| Error | Cause | Fix |
|---|---|---|
| `undefined reference to ...` | Function in anonymous namespace but declared in header | Move definition outside namespace |
| `ambiguous reference to 'X'` | X defined in both .cpp and .hpp | Keep definition in one place |
| `kWidth is not a member of Frame` | Old static constexpr access pattern | Use instance member `f.width` |
| `Segmentation fault` in synth_fm | Frame dimensions mismatched | Check TripleBuffer resize order |

---

## 7. Code Style Notes

- **Namespace:** Everything lives in `namespace famidec { ... }`
- **Include guards:** `#pragma once` is used throughout
- **Forward declarations:** Prefer forward declarations over full includes where possible
- **Thread safety:** `NtscDecoder` is single-producer; `TripleBuffer` is lock-free;
  `Config` is read-only after construction (no mutex needed)
- **AVX2:** DSP hot paths use AVX2 intrinsics; check `dsp/` for vectorized kernels
- **Comments:** Technical comments explain *why*, not *what*. The code is dense and
  assumes familiarity with NTSC composite video theory.

---

## 8. File Index (Quick Reference)

| File | Responsibility |
|---|---|
| `config.hpp` | All user-configurable fields (RF, GUI, resolution, auto-detect) |
| `main.cpp` | CLI, thread setup, render loop, AFC, AGC control |
| `frame.hpp` | Frame struct (RGBA pixels + dimensions), TripleBuffer |
| `ntsc_decoder.cpp` | Core DSP: sync detection, PLL, chroma demod, row rendering |
| `ntsc_decoder.hpp` | Decoder public API + Stats (including auto-detection results) |
| `sdl_display.cpp` | SDL window, renderer, texture, CRT LUT, OSD text rendering |
| `sdl_display.hpp` | SdlDisplay class, OsdStats, CrtLut struct |
| `gui_manager.cpp` | ImGui control panel (channel info, sync status, signal bars) |
| `chroma.hpp` | Burst measurement (phase/amplitude of color subcarrier) |
| `agc.hpp` | Automatic gain control (sync-tip tracking, pedestal clamp) |
| `fir.hpp` | FIR filter design and processing utilities |
| `fm_detector.hpp` | FM quadrature discriminator |
| `sync.hpp` | Sync separator + flywheel PLL for line timing |
| `hackrf_source.cpp` | HackRF One input (10 MSPS, AFC, gain control) |
| `file_source.cpp` | .cs8 recording replay input |
| `synth_fm.cpp` | E2E test: generates synthetic IQ, decodes, asserts RGB |
| `fpv_channels.hpp` | 40 standard 5.8 GHz FPV channel frequencies |
| `spectrum.hpp` | PSD (power spectral density) printing utility |

---

## 9. Auto-Detection Reference

When `--auto-res` is enabled, the decoder populates these stats after lock:

| Stat | Description | NTSC Value | PAL Value |
|---|---|---|---|
| `detected_chroma_hz` | Measured burst subcarrier | ~3.58 MHz | ~4.43 MHz |
| `detected_active_lines` | Lines between vsync | ~240 | ~288 |
| `detected_line_rate` | Hsync frequency (mHz) | ~15734 mHz | ~15625 mHz |
| `detected_active_us` | Active line duration (μs) | ~52.6 μs | ~56.5 μs |
| `detected_horiz_detail` | Max horizontal pixels | ~250 px | ~250 px |
| `auto_detect_ready` | Boolean: detection complete | `true` after ~5 lines | `true` after ~5 lines |

**How `detected_horiz_detail` is calculated:**
```
horiz_detail = chroma_hz × active_us × 1e-6 × 2
```
(The ×2 is the Nyquist factor: 2 samples per cycle.)

This value represents the **theoretical maximum detail** the analog signal
contains. The actual output resolution may be higher (upscaling) or lower
(downscaling), but you cannot render more detail than `detected_horiz_detail`
pixels of actual signal information.

---

## 10. Version History

| Date | Commit | Summary |
|---|---|---|
| 2025-08-08 | 7d736f6 | Configurable resolution + auto-detection |
| 2025-08-08 | ecae62c | Overlay section visibility toggles |
| 2025-08-08 | efdf295 | Transparent overlay refactoring |
| 2025-08-08 | 9b6d3ae | GPSDO CLKIN detection |
| 2025-08-08 | 281e4c4 | README update for ImGui |
| 2025-08-08 | 4ba941a | GuiManager extraction |
| 2025-08-08 | bd54812 | .gitignore updates |
| 2025-08-08 | 2de5355 | ImGui GUI mode |
| 2025-08-08 | 936571f | ImGui mode initial |
| 2025-08-08 | f5498f4 | run.sh personal tweaks |
