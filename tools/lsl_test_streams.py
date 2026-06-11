#!/usr/bin/env python3
"""Synthetic LSL sources for exercising the LSL Stream Viewer.

Launches several concurrent outlets, each targeting a different situation the
viewer must handle. Signal content is chosen so every analysis feature has
something concrete to lock onto:

  eeg         32 ch  @ 500 Hz   float  regular, high-ish channel count.
                                 Per-channel DC offsets (baseline correction),
                                 10 Hz alpha bursts (FFT/band power), 50 Hz line
                                 noise (notch/filtering), pink-ish noise, and
                                 rare large spikes (min/max envelope must keep).
                                 The last few channels are EOG: ~10x amplitude
                                 (slow ocular drift + blinks) so they sit on an
                                 entirely different scale -> tests per-channel
                                 scaling / auto-range that isn't fooled by them.
  highdensity 128 ch @ 2000 Hz  float  regular, stress test for the ring buffer
                                 and the heatmap. A traveling sine wave across
                                 channels -> moving diagonal stripes on a raster.
  chirp       1 ch   @ 1000 Hz  float  linear sweep 1->120 Hz every 8 s. The
                                 reference signal for a spectrogram (a rising
                                 diagonal) and for FFT that moves over time.
  sine        1 ch   @ 1000 Hz  float  pure 40 Hz tone -> one clean FFT peak,
                                 for verifying frequency-axis calibration.
  accel       3 ch   @ 50 Hz    float  regular, low rate / low channel count.
  mouse       2 ch   @ IRREGULAR float  event-driven random walk pushed at
                                 jittered intervals with occasional long
                                 pauses -> exercises the irregular-numeric path.
  markers     1 ch   string IRREGULAR  infrequent (~1/s) event labels.
  fastmarkers 1 ch   string IRREGULAR  frequent (~30/s) trigger codes.

Usage:
    python lsl_test_streams.py                 # all streams
    python lsl_test_streams.py --streams eeg,markers
    python lsl_test_streams.py --eeg-channels 64 --eeg-rate 1000
    python lsl_test_streams.py --hd-channels 304 --hd-rate 8000   # heavy

Stop with Ctrl+C.

Requires: pylsl, numpy.
"""

import argparse
import random
import threading
import time

import numpy as np
from pylsl import StreamInfo, StreamOutlet, local_clock, cf_float32, cf_string, IRREGULAR_RATE

# 10-20 montage labels, extended/cycled to whatever channel count is requested.
_MONTAGE = [
    "Fp1", "Fp2", "F7", "F3", "Fz", "F4", "F8", "FC5", "FC1", "FC2", "FC6",
    "T7", "C3", "Cz", "C4", "T8", "CP5", "CP1", "CP2", "CP6", "P7", "P3",
    "Pz", "P4", "P8", "PO3", "PO4", "O1", "Oz", "O2", "TP9", "TP10",
]


def eeg_labels(n):
    return [_MONTAGE[i] if i < len(_MONTAGE) else f"EEG{i}" for i in range(n)]


def add_channels(info, labels, unit, ctype):
    """Attach per-channel metadata so the viewer can show real channel names."""
    chns = info.desc().append_child("channels")
    for lab in labels:
        ch = chns.append_child("channel")
        ch.append_child_value("label", lab)
        ch.append_child_value("unit", unit)
        ch.append_child_value("type", ctype)


def push_chunk(outlet, chunk, ts):
    """Push a (n_samples, n_channels) float32 array; fall back if numpy is rejected."""
    try:
        outlet.push_chunk(chunk, ts)
    except TypeError:
        outlet.push_chunk(chunk.tolist(), ts)


# ---------------------------------------------------------------------------
# Regular-rate driver: paces a generator callback to a nominal sample rate.
# gen(i0, n) -> (n, channels) float32. Timestamps follow the LSL convention of
# stamping the last sample of the chunk; intermediate samples are inferred from
# the nominal rate by the inlet.
# ---------------------------------------------------------------------------
def run_regular(name, outlet, srate, gen, stop, period=0.02, skew=0.0):
    # `skew` offsets the pushed timestamps (simulates a stream on a differently-set
    # clock); LabRecorder/pyxdf realign via the recorded time-correction offsets.
    start = local_clock()
    pushed = 0
    while not stop.is_set():
        now = local_clock()
        target = int((now - start) * srate)
        n = target - pushed
        if n > 0:
            push_chunk(outlet, gen(pushed, n), now + skew)
            pushed += n
        time.sleep(period)


# ---- generators ------------------------------------------------------------
def make_eeg_gen(C, srate, line_freq, rng, n_eog=2):
    # The last n_eog channels are EOG: a different recording modality sharing the
    # montage but ~10x the amplitude (slow ocular drift + blink deflections).
    eog_idx = list(range(max(0, C - n_eog), C)) if n_eog > 0 else []
    is_eog = np.zeros(C, bool)
    is_eog[eog_idx] = True

    dc = rng.uniform(-2000.0, 2000.0, C).astype(np.float32)      # baseline offsets, uV
    alpha_gain = (15.0 + 10.0 * rng.random(C)).astype(np.float32)
    chan_phase = rng.uniform(0, 2 * np.pi, C).astype(np.float32)
    eog_freq = rng.uniform(0.2, 0.6, len(eog_idx))               # slow ocular drift, Hz
    eog_phase = rng.uniform(0, 2 * np.pi, len(eog_idx))

    def gen(i0, n):
        k = np.arange(i0, i0 + n)
        t = k / srate
        alpha = np.sin(2 * np.pi * 10.0 * t)                     # 10 Hz alpha
        burst = (0.5 * (1 + np.sin(2 * np.pi * 0.1 * t)))        # slow waxing/waning
        line = np.sin(2 * np.pi * line_freq * t)                 # mains interference
        out = dc[None, :].copy()
        out = out + (alpha * burst)[:, None] * alpha_gain[None, :] * np.cos(chan_phase)[None, :]
        out = out + line[:, None] * 8.0
        out = out + rng.standard_normal((n, C)).astype(np.float32) * 6.0
        # Rare large transients (movement artifact): ~2/s, ~500 uV, EEG channels.
        if rng.random() < 2.0 * (n / srate):
            r = rng.integers(0, n)
            c = rng.integers(0, C)
            if not is_eog[c]:
                out[r, c] += rng.choice([-1, 1]) * rng.uniform(400, 800)

        # EOG channels: ~10x scale. Replace EEG content with slow drift + noise.
        for j, c in enumerate(eog_idx):
            slow = np.sin(2 * np.pi * eog_freq[j] * t + eog_phase[j])
            out[:, c] = dc[c] + 300.0 * slow + rng.standard_normal(n).astype(np.float32) * 15.0
        # Blink: a large (~1.2 mV) decaying deflection shared across EOG, ~1 / 3 s.
        if eog_idx and rng.random() < (n / srate) / 3.0:
            r = rng.integers(0, n)
            w = max(1, min(n - r, int(0.15 * srate)))            # ~150 ms
            shape = np.hanning(2 * w)[w:].astype(np.float32)     # peak -> 0 decay
            for c in eog_idx:
                out[r:r + len(shape), c] += 1200.0 * shape
        return out.astype(np.float32)

    return gen, eog_idx


def make_highdensity_gen(C, srate, rng):
    def gen(i0, n):
        k = np.arange(i0, i0 + n)
        t = (k / srate)[:, None]
        c = np.arange(C)[None, :]
        # Traveling plane wave: 6 Hz temporal, phase ramp across channels.
        wave = np.sin(2 * np.pi * 6.0 * t - 0.20 * c)
        out = 50.0 * wave + rng.standard_normal((n, C)) * 4.0
        return out.astype(np.float32)

    return gen


def make_chirp_gen(srate, f0, f1, sweep_s):
    # Continuous phase across chunks via an accumulator (handles any FM cleanly).
    state = {"phase": 0.0}

    def gen(i0, n):
        k = np.arange(i0, i0 + n)
        t = k / srate
        u = (t % sweep_s) / sweep_s
        finst = f0 + (f1 - f0) * u                              # sawtooth sweep
        dphi = 2 * np.pi * finst / srate
        phase = state["phase"] + np.cumsum(dphi)
        state["phase"] = float(phase[-1] % (2 * np.pi))
        return (100.0 * np.sin(phase)).astype(np.float32)[:, None]

    return gen


def make_sine_gen(srate, freq):
    def gen(i0, n):
        k = np.arange(i0, i0 + n)
        return (100.0 * np.sin(2 * np.pi * freq * (k / srate))).astype(np.float32)[:, None]

    return gen


def make_accel_gen(srate, rng):
    def gen(i0, n):
        k = np.arange(i0, i0 + n)
        t = (k / srate)[:, None]
        base = np.array([0.0, 0.0, 1.0])                        # gravity on Z, g units
        out = base[None, :] + 0.3 * np.sin(2 * np.pi * np.array([1.1, 1.7, 0.5])[None, :] * t)
        out = out + rng.standard_normal((n, 3)) * 0.02
        return out.astype(np.float32)

    return gen


# ---- irregular streams -----------------------------------------------------
def run_mouse(outlet, stop, rng):
    x, y = 960.0, 540.0
    while not stop.is_set():
        # Jittered inter-sample interval, with occasional long idle pauses.
        dt = rng.exponential(0.016)
        if rng.random() < 0.03:
            dt += rng.uniform(0.3, 1.5)
        time.sleep(min(dt, 2.0))
        if stop.is_set():
            break
        x = float(np.clip(x + rng.normal(0, 25), 0, 1920))
        y = float(np.clip(y + rng.normal(0, 25), 0, 1080))
        outlet.push_sample([x, y], local_clock())


def run_markers(outlet, stop, rng, labels, rate_hz, skew=0.0):
    while not stop.is_set():
        time.sleep(rng.exponential(1.0 / rate_hz))
        if stop.is_set():
            break
        outlet.push_sample([random.choice(labels)], local_clock() + skew)


# A regular stream that repeatedly DISCONNECTS: it destroys its outlet, stays gone
# for a few seconds, then comes back (same source_id) so an inlet with recover=True
# reconnects. Use it to see recovery + the "temporarily missing" indicator.
def run_flaky(suffix, stop, rng, up=8.0, down=3.0):
    sr, C = 100, 4
    def gen(i0, n):
        k = np.arange(i0, i0 + n)
        t = (k / sr)[:, None]
        c = np.arange(C)[None, :]
        out = 40.0 * np.sin(2 * np.pi * (3.0 + c) * t) + rng.standard_normal((n, C)) * 3.0
        return out.astype(np.float32)

    while not stop.is_set():
        info = StreamInfo(f"MockFlaky{suffix}", "EEG", C, sr, cf_float32, "mock-flaky")
        add_channels(info, [f"flk{i}" for i in range(C)], "microvolts", "EEG")
        out = StreamOutlet(info, chunk_size=int(sr * 0.02), max_buffered=360)
        start = local_clock(); pushed = 0
        while not stop.is_set() and (local_clock() - start) < up:   # "up": stream data
            now = local_clock()
            n = int((now - start) * sr) - pushed
            if n > 0:
                push_chunk(out, gen(pushed, n), now); pushed += n
            time.sleep(0.02)
        del out                                                     # disconnect
        t = local_clock()
        while not stop.is_set() and (local_clock() - t) < down:     # stay gone
            time.sleep(0.1)


# ---------------------------------------------------------------------------
def run_evoked(dout, mout, stop, rng, sr=250):
    """Evoked-response demo: a data channel where every 'stim' marker is followed by
    a P100-like bump (~120 ms), and 'target' trials additionally get a P300 (~300 ms).
    Use the ERP window: trigger on this marker stream, match 'target' vs 'standard'."""
    start = local_clock()
    pushed = 0
    onsets = []                                   # (onset_sample_index, is_target)
    next_stim = start + rng.uniform(0.6, 1.0)
    while not stop.is_set():
        now = local_clock()
        if now >= next_stim:
            target = bool(rng.random() < 0.4)
            mout.push_sample(["target" if target else "standard"], now)
            onsets.append((pushed, target))
            next_stim = now + rng.uniform(0.6, 1.0)
        n = int((now - start) * sr) - pushed
        if n > 0:
            k = np.arange(pushed, pushed + n)
            sig = (rng.standard_normal(n) * 4.0).astype(np.float32)   # background noise
            for (o, tgt) in onsets:
                sig += 6.0 * np.exp(-0.5 * ((k - (o + 0.12 * sr)) / (0.025 * sr)) ** 2)   # P100
                if tgt:
                    sig += 10.0 * np.exp(-0.5 * ((k - (o + 0.30 * sr)) / (0.05 * sr)) ** 2)  # P300
            push_chunk(dout, sig.reshape(-1, 1).astype(np.float32), now)
            pushed += n
            onsets = [(o, t) for (o, t) in onsets if o > pushed - sr]   # prune past bumps
        time.sleep(0.01)


def build_streams(selected, args, stop, host_suffix):
    rng = np.random.default_rng(0xC0FFEE)
    threads = []
    started = []

    def spawn(target, *a):
        th = threading.Thread(target=target, args=a, daemon=True)
        th.start()
        threads.append(th)

    # Per-stream timestamp skew (s) when --clock-skew is given, so streams sit on
    # distinct apparent clocks (the recorder's time-correction chunks realign them).
    _order = ["eeg", "accel", "sine", "markers", "chirp", "hd", "mouse"]
    def sk(name):
        base = getattr(args, "clock_skew", 0.0) or 0.0
        if base == 0.0:
            return 0.0
        return round((_order.index(name) - 2 if name in _order else 0) * base, 4)

    if "eeg" in selected:
        C, sr = args.eeg_channels, args.eeg_rate
        n_eog = min(args.eeg_eog, C)
        labels = eeg_labels(C - n_eog) + [f"EOG{i + 1}" for i in range(n_eog)]
        info = StreamInfo(f"MockEEG{host_suffix}", "EEG", C, sr, cf_float32, "mock-eeg")
        add_channels(info, labels, "microvolts", "EEG")
        out = StreamOutlet(info, chunk_size=int(sr * 0.02), max_buffered=360)
        gen, _ = make_eeg_gen(C, sr, args.line_freq, rng, n_eog)
        spawn(run_regular, "eeg", out, sr, gen, stop, 0.02, sk("eeg"))
        started.append(f"eeg          {C}ch @ {sr:g} Hz  float  ({n_eog} EOG @ ~10x scale)")

    if "highdensity" in selected:
        C, sr = args.hd_channels, args.hd_rate
        info = StreamInfo(f"MockHighDensity{host_suffix}", "EEG", C, sr, cf_float32, "mock-hd")
        add_channels(info, [f"ch{i}" for i in range(C)], "microvolts", "EEG")
        out = StreamOutlet(info, chunk_size=int(sr * 0.01), max_buffered=360)
        spawn(run_regular, "hd", out, sr, make_highdensity_gen(C, sr, rng), stop)
        started.append(f"highdensity  {C}ch @ {sr:g} Hz  float")

    if "chirp" in selected:
        sr = 1000
        info = StreamInfo(f"MockChirp{host_suffix}", "Signal", 1, sr, cf_float32, "mock-chirp")
        add_channels(info, ["chirp"], "au", "misc")
        out = StreamOutlet(info)
        spawn(run_regular, "chirp", out, sr, make_chirp_gen(sr, 1.0, 120.0, 8.0), stop)
        started.append(f"chirp        1ch @ {sr} Hz  float  (1->120 Hz sweep)")

    if "sine" in selected:
        sr = 1000
        info = StreamInfo(f"MockSine40{host_suffix}", "Signal", 1, sr, cf_float32, "mock-sine")
        add_channels(info, ["sine40"], "au", "misc")
        out = StreamOutlet(info)
        spawn(run_regular, "sine", out, sr, make_sine_gen(sr, 40.0), stop, 0.02, sk("sine"))
        started.append(f"sine         1ch @ {sr} Hz  float  (pure 40 Hz)")

    if "accel" in selected:
        sr = 50
        info = StreamInfo(f"MockAccel{host_suffix}", "Accelerometer", 3, sr, cf_float32, "mock-accel")
        add_channels(info, ["acc_x", "acc_y", "acc_z"], "g", "Accelerometer")
        out = StreamOutlet(info)
        spawn(run_regular, "accel", out, sr, make_accel_gen(sr, rng), stop, 0.02, sk("accel"))
        started.append(f"accel        3ch @ {sr} Hz  float")

    if "mouse" in selected:
        info = StreamInfo(f"MockMouse{host_suffix}", "Position", 2, IRREGULAR_RATE,
                          cf_float32, "mock-mouse")
        add_channels(info, ["mouse_x", "mouse_y"], "pixels", "Position")
        out = StreamOutlet(info)
        spawn(run_mouse, out, stop, rng)
        started.append("mouse        2ch @ irregular  float")

    if "flaky" in selected:
        spawn(run_flaky, host_suffix, stop, rng)
        started.append("flaky        4ch @ 100 Hz  float  (disconnects ~3 s every ~8 s)")

    if "markers" in selected:
        labels = ["stim/left", "stim/right", "response/correct",
                  "response/error", "block/start", "block/end"]
        info = StreamInfo(f"MockMarkers{host_suffix}", "Markers", 1, IRREGULAR_RATE,
                          cf_string, "mock-markers")
        out = StreamOutlet(info)
        spawn(run_markers, out, stop, rng, labels, 1.0, sk("markers"))
        started.append("markers      1ch @ irregular  string (~1/s)")

    if "fastmarkers" in selected:
        labels = [f"T{i}" for i in range(1, 9)]
        info = StreamInfo(f"MockFastMarkers{host_suffix}", "Markers", 1, IRREGULAR_RATE,
                          cf_string, "mock-fastmarkers")
        out = StreamOutlet(info)
        spawn(run_markers, out, stop, rng, labels, 30.0)
        started.append("fastmarkers  1ch @ irregular  string (~30/s)")

    if "evoked" in selected:
        sr = 250
        dinfo = StreamInfo(f"MockEvoked{host_suffix}", "EEG", 1, sr, cf_float32, "mock-evoked")
        add_channels(dinfo, ["Cz"], "microvolts", "EEG")
        dout = StreamOutlet(dinfo)
        minfo = StreamInfo(f"MockEvokedMarkers{host_suffix}", "Markers", 1, IRREGULAR_RATE,
                           cf_string, "mock-evoked-markers")
        mout = StreamOutlet(minfo)
        spawn(run_evoked, dout, mout, stop, rng, sr)
        started.append("evoked       1ch @ 250 Hz + markers  (P100 always, P300 on 'target'; ERP demo)")

    return threads, started


ALL = ["eeg", "highdensity", "chirp", "sine", "accel", "flaky", "mouse",
       "markers", "fastmarkers", "evoked"]


def main():
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--streams", default="all",
                   help="comma list from: " + ",".join(ALL) + " (default: all)")
    p.add_argument("--eeg-channels", type=int, default=32)
    p.add_argument("--eeg-rate", type=float, default=500.0)
    p.add_argument("--eeg-eog", type=int, default=2,
                   help="number of trailing EEG channels emulated as ~10x-scale EOG")
    p.add_argument("--hd-channels", type=int, default=128)
    p.add_argument("--hd-rate", type=float, default=2000.0)
    p.add_argument("--line-freq", type=float, default=50.0,
                   help="mains interference frequency in the EEG stream (50 or 60)")
    p.add_argument("--clock-skew", type=float, default=0.0,
                   help="give streams distinct timestamp offsets (s) to exercise clock sync")
    p.add_argument("--suffix", default="",
                   help="appended to every stream name (run multiple senders apart)")
    args = p.parse_args()

    selected = ALL if args.streams == "all" else [s.strip() for s in args.streams.split(",")]
    unknown = [s for s in selected if s not in ALL]
    if unknown:
        p.error(f"unknown stream(s): {', '.join(unknown)}")

    stop = threading.Event()
    _, started = build_streams(selected, args, stop, args.suffix)

    print("LSL test sources running. Streams:")
    for line in started:
        print("  " + line)
    print("Press Ctrl+C to stop.")
    try:
        while True:
            time.sleep(1.0)
    except KeyboardInterrupt:
        print("\nstopping...")
        stop.set()
        time.sleep(0.3)


if __name__ == "__main__":
    main()
