# LSL Stream Viewer — Design Brief

Cross-platform desktop app for visualising Lab Streaming Layer streams in real time.
Stack: **SDL3 + SDL_GPU · Dear ImGui (docking) · ImPlot · liblsl C++ API**.
Target: Windows / macOS (incl. Apple Silicon) / Linux. C++17.

---

## Files in this repo

| File | Status | Notes |
|------|--------|-------|
| `CMakeLists.txt` | working skeleton | FetchContent: SDL3, liblsl, imgui (docking), implot |
| `main.cpp` | working skeleton | SDL3/GPU loop + basic stream browser + old plot path |
| `lsl_source.hpp` | superseded (keep for Discovery) | Per-sample StreamSource + simple StreamBuffer — too slow for HF |
| `magic_ring_buffer.hpp` | complete | Cross-platform mirrored ring + InterleavedRing SPSC |
| `minmax_summary.hpp` | complete | Incremental absolute-indexed min/max for shimmer-free decimation |
| `decimate.hpp` | superseded | Window-relative decimator — shimmers on scroll, replaced by MinMaxSummary |

---

## Architecture

### Data flow

```
LSL network
    │
    └─ producer thread (one per stream)
           pull_chunk_multiplexed → preallocated flat float buffer
           ring.write(buf, n)          ← single memcpy into InterleavedRing
           summary.append(buf, n)      ← fold into MinMaxSummary (cache-hot)
                │
                ▼
         InterleavedRing          MinMaxSummary
         (magic ring buffer)      (fixed-grid min/max bins)
                │                        │
                └──────────┬─────────────┘
                           ▼
                    render thread (vsync)
                    MinMaxSummary::read() → PlotShaded   (normal view)
                    InterleavedRing::recent()             (zoomed-in / FFT)
                    analysis thread → FFT scratch → ImPlot (spectrogram)
```

### Ring buffer — `magic_ring_buffer.hpp`

`MagicRingBuffer` maps the same physical pages twice, contiguously in virtual memory,
so any read or write of ≤ `bytes()` starting anywhere in `[0, bytes())` is
always contiguous — no wrap branch, no split memcpy.

`InterleavedRing` builds typed SPSC semantics on top:
- Layout: sample-major interleaved `[s0c0 s0c1 … | s1c0 …]` — matches the LSL
  multiplexed chunk layout so the producer does exactly **one memcpy per chunk**.
- Monotonic `uint64_t head` (acquire/release). Readers snapshot `head`, read
  most-recent `window` samples. No tail needed for a live viewer.
- Sizing: `init(channels, min_history_samples)` computes
  `unit = lcm(mapping_granularity, channels * sizeof(float))` and rounds up.
  **This is load-bearing**: 304 ch × 4 B = 1216 bytes does not divide a 4 KB
  page (or 16 KB on Apple Silicon, or 64 KB on Windows), so the physical mirror
  seam and the sample-wrap must be aligned via lcm or the ring silently corrupts
  at every wraparound.

Platform notes:
- **Linux**: `memfd_create` + two `MAP_FIXED` maps over a `PROT_NONE` reservation.
- **macOS**: `shm_open` with a unique pid-scoped name, immediately `shm_unlink`
  (effectively anonymous), same double-map. Apple Silicon page = 16 KB —
  handled by `sysconf(_SC_PAGESIZE)`.
- **Windows**: `CreateFileMapping` + `VirtualAlloc2` placeholder +
  two `MapViewOfFile3` with `MEM_REPLACE_PLACEHOLDER`.
  Requires `_WIN32_WINNT >= 0x0A00` (Win 10 1803+) and **`onecore.lib`**.
  Allocation granularity = 64 KB. MinGW may lack the `VirtualAlloc2` import
  lib — prefer MSVC or clang-cl on Windows.

### Decimation — `minmax_summary.hpp`

Window-relative bucketing shimmers on scroll: the bucket grid is anchored to
the moving window start, so a feature's extreme sample crosses bucket boundaries
each frame, causing per-pixel height flicker. It also rescans ~80k × 304 samples
per frame with 1216-byte strides (cache-hostile).

`MinMaxSummary` uses a **fixed grid keyed to absolute sample index**: bin `k`
always covers absolute samples `[k·B, (k+1)·B)`. A closed bin's `(min, max)` and
its time are immutable. A scrolling view therefore **translates** the bins — no
re-derivation, no shimmer — and per-frame reader work drops to `visible_bins × C`.

- `append(float* chunk, size_t n)` — called by producer immediately after
  `ring.write()` on the same buffer (cache-hot). Folds per sample; commits a bin
  atomically every `B` samples.
- `read(int ch, size_t bins, double dt, double t0, float* x, float* mn, float* mx)`
  — returns the most-recent `bins` closed bins for one channel, ready for
  `ImPlot::PlotShaded`.
- Live edge lags by ≤ `B` samples (e.g. 32/8000 Hz = 4 ms). Negligible.
- Choose `B` so that at maximum zoom-out ~1 bin/pixel
  (e.g. 10 s window, 8 kHz, 1500 px wide → B ≈ 32–64).
- For a wide zoom range keep a small pyramid of levels (B, 4B, 16B …) and pick
  the level nearest 1 bin/pixel. Currently only one level is implemented.

### LSL ingestion

Use `pull_chunk_multiplexed` with a **preallocated** flat buffer:

```cpp
// return value is channel-VALUES written; samples = got / channels
std::size_t got = inlet.pull_chunk_multiplexed(
    buf.data(), ts.data(), buf.size(), ts.size(), 0.5 /*timeout*/);
std::size_t n = got / C;
ring.write(buf.data(), n);
summary.append(buf.data(), n);
```

Set inlet `max_chunklen` and sender chunking to 5–30 ms. For a regular stream,
**do not store per-sample timestamps** — derive x from absolute sample index +
srate using one `(sample_index ↔ LSL_timestamp)` anchor refreshed periodically.
Store timestamps only for marker / irregular-rate streams.

`time_correction()` maps the sender's LSL clock to the local clock; call it once
on connect and refresh every ~5 s for cross-stream alignment.

### SDL_GPU render loop (critical note)

`ImGui_ImplSDLGPU3_PrepareDrawData()` is **mandatory before** `SDL_BeginGPURenderPass`.
SDL_GPU forbids copy ops (vertex/index buffer upload) inside a render pass.
The ordering in `main.cpp` is correct:

```
SDL_AcquireGPUCommandBuffer
SDL_WaitAndAcquireGPUSwapchainTexture
ImGui_ImplSDLGPU3_PrepareDrawData(draw_data, cmd)   ← BEFORE BeginGPURenderPass
SDL_BeginGPURenderPass
ImGui_ImplSDLGPU3_RenderDrawData(draw_data, cmd, pass)
SDL_EndGPURenderPass
SDL_SubmitGPUCommandBuffer
```

---

## What still needs to be built

### High-priority / core

- [ ] **New `HfStreamSource`** — replace the per-sample `StreamSource` in
  `lsl_source.hpp` with a producer that uses `InterleavedRing` + `MinMaxSummary`
  and pulls via `pull_chunk_multiplexed`. Wire `Discovery` (keep as-is) into it.
- [ ] **Update `main.cpp` display path** — replace the old `StreamBuffer`/`PlotLine`
  plot with `MinMaxSummary::read()` + `ImPlot::PlotShaded` for the envelope.
  Keep `InterleavedRing::recent()` + raw `PlotLine` for the zoomed-in path
  (when samples-per-pixel ≤ B).
- [ ] **Time axis anchor** — map absolute sample index to wall-clock seconds using
  the `(sample_index, lsl_local_timestamp)` anchor + `time_correction()` offset.
- [ ] **Channel names** — parse `stream_info.desc()` XML for per-channel labels;
  fall back to `ch0 … chN`.

### Marker / irregular streams

- [ ] Detect `nominal_srate() == 0` or `channel_format() == cf_string`.
- [ ] Small timestamp ring for irregular numeric streams.
- [ ] String marker ring (separate from float ring).
- [ ] Render markers as `ImPlot::PlotInfLines` / annotations overlaid on signal plots.

### 304-channel visualisation

- [ ] **Heatmap / raster view** — channels on Y, time on X, amplitude as colour.
  `ImPlot::PlotHeatmap` for moderate resolution, or a scrolling `SDL_GPUTexture`
  updated column-by-column (ring) for full rate. Primary view for dense arrays.
- [ ] Focus-channel selection — user picks a subset (e.g. 8–16) for full
  min/max trace view stacked with per-channel vertical offset.
- [ ] Stack/offset toggle: add `c * offset_uV` to the envelope y for EEG-style display.

### Supplementary analysis

- [ ] **FFT plot** — per selected channel, `ring.recent()` → Hann window → real FFT
  → `PlotLine` of magnitude spectrum. Library: **pffft** (SIMD, BSD, tiny) or
  **KissFFT** (simpler API). Run on a dedicated analysis thread; publish results
  via a small double-buffer.
- [ ] **Spectrogram** — STFT feeding a scrolling texture (one new column per hop).
  Upload via SDL_GPU `SDL_UploadToGPUTexture` or `ImPlot::PlotHeatmap` for
  single-channel focus. Hann window, 50% overlap.

### Min/max pyramid (zoom)

- [ ] Add a second summary level at `4B` samples/bin (and optionally `16B`) so
  zooming out further still gives ~1 bin/pixel without re-scanning the raw ring.
  `MinMaxSummary::read()` already uses absolute bin indices; adding levels is
  additive.

### Build / platform

- [ ] Pin `GIT_TAG` values in `CMakeLists.txt` to current stable releases (SDL3
  `release-3.2.x`, liblsl latest tag).
- [ ] Test `MagicRingBuffer` on Windows (onecore.lib link, teardown path).
- [ ] Test on Apple Silicon (16 KB page, lcm sizing).
- [ ] Add a CI matrix: ubuntu / macos / windows.

---

## Key invariants to preserve

1. The LSL producer thread is the **only writer** to `InterleavedRing` and
   `MinMaxSummary`. Never write from the render thread.
2. `InterleavedRing::init()` must use the lcm-rounded size — do not bypass it.
3. `PrepareDrawData` must stay before `BeginGPURenderPass`.
4. The `MinMaxSummary` bin grid is anchored to absolute sample index zero of each
   stream — do not reset `binAbs_` or `closed_` after construction.
5. For regular-rate streams, x is computed from sample index, not stored
   per sample — do not add a timestamp array to `InterleavedRing`.
