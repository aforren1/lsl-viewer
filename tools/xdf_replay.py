#!/usr/bin/env python3
# /// script
# requires-python = ">=3.9"
# dependencies = ["pylsl>=1.16", "numpy"]
# ///
"""Replay the streams of an XDF recording as live LSL outlets.

Each stream in the file becomes an outlet with the recorded name, type,
channel count, nominal rate, channel format, source_id, and the full <desc>
subtree (channel labels, units, locations, ...). All outlets share one
playback clock, so the relative timing between streams is the same as in the
recording. Time zero is the first sample in the file.

The file is streamed: a reader thread decodes the chunks of the selected
streams in order of their start time (file order for gzip) and stays about
--lookahead seconds ahead of playback.
Memory use depends on the read-ahead, not on the size of the file, and
playback starts within a second also for multi-GB files. --start seeks
through an index of the file. A chunk that the reader delivers after its time
is pushed at once with its recorded timestamps; the number of such late
chunks is printed when playback ends.

Timestamps are the recorded ones, shifted onto local_clock() (and divided by
--speed). Regular streams go out in chunks of about --chunk-ms; irregular
streams (markers, events) go out at the time of each sample. With --loop,
each pass is shifted by the window length, so timestamps keep increasing.

Clock sync (default on) and --dejitter reproduce pyxdf.load_xdf with its
default settings, so the replayed timestamps are the ones pyxdf would return.
Sync is on by default because streams recorded from other hosts are on
unrelated clocks without it. Dejitter is off by default, so the replay sends
the timestamps the recorder received, jitter included, as a live inlet would
see them. --dejitter first reads all timestamps of the selected streams, which
takes a few seconds on large files.

A damaged file still plays. If the chunk structure is broken, the data up to
the next Boundary chunk (about 10 s in LabRecorder files) is lost. A single
chunk whose content is bad is skipped. A cut-off file plays up to the cut.

A stream with no samples (in the file or in the window) still gets an outlet,
so the stream list matches the recording.

At --speed other than 1 the effective rate is nominal rate x speed; the
nominal rate in the metadata is not changed.

Usage:
    uv run tools/xdf_replay.py rec.xdf --list            # show streams, no replay
    uv run tools/xdf_replay.py rec.xdf                   # replay all streams once
    uv run tools/xdf_replay.py rec.xdf --loop --suffix _replay
    uv run tools/xdf_replay.py rec.xdf --streams 2,type:Markers --start 60 --duration 30
    uv run tools/xdf_replay.py rec.xdf --streams "Mock*" --speed 2

Stream selection (--streams) is a comma list. Each item is a stream id (as
printed by --list), "type:<pattern>", or a name pattern. Patterns use
shell-style wildcards (* and ?). The reader only reads the chunks of the
selected streams.

Stop with Ctrl+C.

Requires: pylsl, numpy.
"""

import argparse
import ctypes
import fnmatch
import gzip
import bisect
import heapq
import os
import queue
import struct
import sys
import threading
import time
import xml.etree.ElementTree as ET

import numpy as np
from numpy.lib.stride_tricks import as_strided
from pylsl import StreamInfo, StreamOutlet, local_clock
from pylsl.lib import lib

lib.lsl_streaminfo_from_xml.restype = ctypes.c_void_p
lib.lsl_streaminfo_from_xml.argtypes = [ctypes.c_char_p]

_NUMERIC_DTYPES = {
    "float32": np.float32, "double64": np.float64, "int8": np.int8,
    "int16": np.int16, "int32": np.int32, "int64": np.int64,
}
_F64 = struct.Struct("<d")
_U32 = struct.Struct("<I")
_U64 = struct.Struct("<Q")
# Seconds of slack around the window when picking chunks by their first
# timestamp, because raw timestamps can step backwards across chunk borders.
_MARGIN = 1.0


def _warn(msg):
    print(f"warning: {msg}", file=sys.stderr, flush=True)


# ---------------------------------------------------------------------------
# Header scan and chunk index. It reads only chunk headers, stream headers,
# footers, clock offsets, and the first bytes of each Samples chunk, so it is
# fast even on multi-GB files. The raw header XML is kept because a dict form
# would drop attributes and the order of differently named siblings.
# ---------------------------------------------------------------------------
def _open(path):
    """Return (file, size); size is None for gzip, where it is unknown up front."""
    with open(path, "rb") as f:
        gz = f.read(2) == b"\x1f\x8b"
    if gz:
        return gzip.open(path, "rb"), None
    return open(path, "rb", buffering=1 << 16), os.path.getsize(path)


def _varlen(f):
    nb = f.read(1)
    if not nb:
        raise EOFError
    n = nb[0]
    if n == 1:
        return f.read(1)[0]
    if n == 4:
        return _U32.unpack(f.read(4))[0]
    if n == 8:
        return _U64.unpack(f.read(8))[0]
    raise ValueError(f"invalid length-byte count {n} at offset {f.tell() - 1}")


def _varlen_buf(buf, q):
    n = buf[q]
    if n == 1:
        return buf[q + 1], q + 2
    if n == 4:
        return _U32.unpack_from(buf, q + 1)[0], q + 5
    if n == 8:
        return _U64.unpack_from(buf, q + 1)[0], q + 9
    raise ValueError(f"invalid length-byte count {n}")


def _last_timestamp(f, offset, fmt, nch, srate):
    """Decode one Samples chunk just far enough to get its last timestamp."""
    f.seek(offset)
    count = _varlen(f)
    size = 0 if fmt == "string" else np.dtype(_NUMERIC_DTYPES[fmt]).itemsize * nch
    ts = None
    for _ in range(count):
        if f.read(1)[0]:
            ts = _F64.unpack(f.read(8))[0]
        elif ts is not None and srate > 0:
            ts += 1.0 / srate
        if size:
            f.seek(size, 1)
        else:
            for _ in range(nch):
                f.seek(_varlen(f), 1)
    return ts


# Recorders write a Boundary chunk (tag 5) with this body every few seconds, so
# a reader that hits damage can find its place again, as pyxdf does.
_BOUNDARY = bytes([0x43, 0xA5, 0x46, 0xDC, 0xCB, 0xF5, 0x41, 0x0F,
                   0xB3, 0x0E, 0xD5, 0x46, 0x73, 0x83, 0xCB, 0xE4])


def _scan_forward(f, pos):
    """Seek to just after the next Boundary signature at or after pos.
    Returns the new position, or None at the end of the file."""
    f.seek(pos)
    tail = b""
    while True:
        block = f.read(1 << 20)
        if not block:
            return None
        # Keep the end of the previous block so a signature across the seam is found.
        buf = tail + block
        hit = buf.find(_BOUNDARY)
        if hit != -1:
            end = f.tell() - len(buf) + hit + len(_BOUNDARY)
            f.seek(end)
            return end
        tail = buf[-(len(_BOUNDARY) - 1):]


def scan_xdf(path):
    """Return {stream_id: header dict} with a per-stream index of Samples chunks.

    Index arrays per stream: off (file offset after the stream id), end (end of
    the chunk), n (samples), gstart (index of the chunk's first sample in the
    stream), ref (raw timestamp of the first sample, NaN if unknown), first_exp
    (first sample has its own timestamp), has_exp (any sample has one).
    """
    streams = {}
    chunks = {}
    f, size = _open(path)
    with f:
        if f.read(4) != b"XDF:":
            raise ValueError(f"{path}: not an XDF file")
        while True:
            start = f.tell()
            try:
                length = _varlen(f)
                body = f.tell()
                if size is not None and body + length > size:
                    raise IndexError("chunk runs past the end of the file")
                tag = struct.unpack("<H", f.read(2))[0]
                if tag in (2, 3, 4, 6):
                    sid = _U32.unpack(f.read(4))[0]
                if tag == 2:
                    root = ET.fromstring(f.read(length - 6).decode("utf-8", "replace"))
                    fmt = root.findtext("channel_format")
                    nch = int(root.findtext("channel_count"))
                    srate = float(root.findtext("nominal_srate") or 0)
                    streams[sid] = {
                        "id": sid,
                        "name": root.findtext("name") or "",
                        "type": root.findtext("type") or "",
                        "nch": nch, "srate": srate, "fmt": fmt,
                        "source_id": root.findtext("source_id") or "",
                        "xml": root,
                        "vsize": 0 if fmt == "string"
                        else np.dtype(_NUMERIC_DTYPES[fmt]).itemsize * nch,
                        "tdiff": 1.0 / srate if srate > 0 else 0.0,
                        "count": 0, "first": None, "last": None, "footer_count": None,
                        "clock_times": [], "clock_values": [],
                    }
                    chunks[sid] = []
                elif tag == 3 and sid in streams:
                    s = streams[sid]
                    off = f.tell()
                    n = _varlen(f)
                    rest = body + length - f.tell()
                    ref, first_exp, has_exp = np.nan, False, False
                    if n:
                        t = f.read(1)[0]
                        first_exp = t != 0
                        if s["vsize"]:
                            full = 9 + s["vsize"]
                            k, r = divmod(n * full - rest, 8)
                            if r or not 0 <= k <= n:
                                raise ValueError("sample sizes do not match the chunk length")
                            has_exp = k < n
                            # Leading samples without a timestamp have a fixed size, so
                            # the first stamped one is a few seeks away.
                            j = 0
                            while not t and j < min(n - 1, 64):
                                f.seek(s["vsize"], 1)
                                t = f.read(1)[0]
                                j += 1
                            if t:
                                ref = _F64.unpack(f.read(8))[0] - j * s["tdiff"]
                        else:
                            has_exp = first_exp
                            if t:
                                ref = _F64.unpack(f.read(8))[0]
                        if s["first"] is None:
                            # pyxdf starts each stream's deduced timestamps from 0.0.
                            s["first"] = ref if first_exp else s["tdiff"]
                    s["count"] += n
                    chunks[sid].append((off, body + length, n, ref, first_exp, has_exp))
                elif tag == 4 and sid in streams:
                    t, v = struct.unpack("<dd", f.read(16))
                    streams[sid]["clock_times"].append(t)
                    streams[sid]["clock_values"].append(v)
                elif tag == 6 and sid in streams:
                    try:
                        foot = ET.fromstring(f.read(length - 6))
                        cnt = foot.findtext("sample_count")
                        streams[sid]["footer_count"] = int(cnt) if cnt else None
                    except (ET.ParseError, ValueError):
                        pass
            except EOFError:
                break
            except (struct.error, IndexError, ValueError, ET.ParseError):
                # A recorder that crashed leaves a cut-off last chunk. Damage in the
                # middle loses only the data up to the next Boundary chunk.
                resume = _scan_forward(f, start + 1)
                if resume is None:
                    _warn(f"damaged or cut-off chunk near byte {start}, file read up to there")
                    break
                _warn(f"damaged chunk near byte {start}, skipped to the boundary at byte {resume}")
                continue
            f.seek(body + length)
        for sid, s in streams.items():
            c = chunks[sid]
            if c:
                try:
                    s["last"] = _last_timestamp(f, c[-1][0], s["fmt"], s["nch"], s["srate"])
                except (struct.error, IndexError, TypeError, ValueError):
                    s["last"] = None
            cols = list(zip(*c)) if c else [()] * 6
            s["off"] = np.array(cols[0], dtype=np.int64)
            s["end"] = np.array(cols[1], dtype=np.int64)
            s["n"] = np.array(cols[2], dtype=np.int64)
            s["gstart"] = np.concatenate(([0], np.cumsum(s["n"])[:-1])).astype(np.int64)[:len(c)]
            s["ref"] = np.array(cols[3], dtype=np.float64)
            s["first_exp"] = np.array(cols[4], dtype=bool)
            s["has_exp"] = np.array(cols[5], dtype=bool)
    return streams


def _chunk_starts(raw, g, tdiff):
    """Start time of every chunk, usable to seek and to pace the reader.

    raw holds clock-corrected first stamps (raw clocks can reset backwards).
    A chunk can have no stamp near its start, and a damaged chunk with an
    intact length yields a nonsense one. Either would make --start pick the
    wrong chunk or stall the reader, so only starts on the longest
    non-decreasing run are kept (gaps in the recording only move forward), and
    the others are extrapolated from the last kept one.
    """
    keep = np.zeros(len(raw), bool)
    fin = np.flatnonzero(np.isfinite(raw))
    if len(fin):
        tails, tail_at, parent = [], [], [-1] * len(fin)
        for i, v in enumerate(raw[fin].tolist()):
            k = bisect.bisect_right(tails, v)
            if k == len(tails):
                tails.append(v)
                tail_at.append(i)
            else:
                tails[k] = v
                tail_at[k] = i
            parent[i] = tail_at[k - 1] if k else -1
        i = tail_at[-1]
        while i >= 0:
            keep[fin[i]] = True
            i = parent[i]
    if keep.all():
        return raw
    prev = np.maximum.accumulate(np.where(keep, np.arange(len(raw)), -1))
    p = np.maximum(prev, 0)
    # pyxdf deduces a stream's leading unstamped samples from 0.0.
    return np.where(prev >= 0, raw[p] + (g - g[p]) * tdiff, (g + 1) * tdiff)


def _plausible(ts, ref, c):
    """False when a decoded chunk's stamps lie outside its neighbors' starts:
    misaligned bytes after damage can still pass the chunk's size checks."""
    t = ts[np.isfinite(ts)]
    if not len(t):
        return True
    upper = ref[c + 1] + _MARGIN if c + 1 < len(ref) else np.inf
    return bool(t.min() >= ref[c] - _MARGIN and t.max() <= upper)


# ---------------------------------------------------------------------------
# Clock sync and dejitter. These port the algorithms of pyxdf 1.17
# (_truncate_corrupted_offsets, _clock_sync, _robust_fit, _jitter_removal)
# with its default parameters, operation for operation, so that the replayed
# timestamps match pyxdf.load_xdf to the last bit. pyxdf notice:
#
# BSD 2-Clause License
# Copyright (c) 2015-2024, Syntrogi Inc. dba Intheon
# Copyright (c) 2018-2024, Chad Boulay
# Copyright (c) 2018-2024, Tristan Stenner
# Copyright (c) 2018-2024, Clemens Brunner
# Redistribution and use in source and binary forms, with or without
# modification, are permitted provided that the following conditions are met:
# * Redistributions of source code must retain the above copyright notice,
#   this list of conditions and the following disclaimer.
# * Redistributions in binary form must reproduce the above copyright notice,
#   this list of conditions and the following disclaimer in the documentation
#   and/or other materials provided with the distribution.
# THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
# AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
# IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
# DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
# FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
# DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
# SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
# CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
# OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
# OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
# ---------------------------------------------------------------------------
_WINSOR = 0.0001


def _segment_indices(b_breaks):
    break_inds = np.where(b_breaks)[0]
    start_idx = np.hstack(([0], break_inds + 1))
    end_idx = np.hstack((break_inds, len(b_breaks)))
    return list(zip(start_idx.tolist(), end_idx.tolist()))


def _clock_glitch(diff, thresh_stds, thresh_secs):
    median = np.median(diff)
    shift = diff - median
    shift_abs = np.abs(shift)
    mad = np.median(shift_abs) + np.finfo(float).eps
    return (np.abs(shift / mad) > thresh_stds) & (shift_abs > thresh_secs)


def _last_offset_corrupted(times, values):
    times, values = np.asarray(times), np.asarray(values)
    if len(times) < 3:
        return False
    intervals = np.diff(times)
    median_interval = np.median(intervals[:-1])
    last_interval = np.abs(intervals[-1])
    if median_interval > 0:
        time_ratio = last_interval / median_interval
    else:
        time_ratio = np.inf if last_interval > 0 else 1.0
    median_val = np.median(values[:-1])
    mad = np.median(np.abs(values[:-1] - median_val))
    zscore = np.abs(values[-1] - median_val) / (1.4826 * mad) if mad > np.finfo(float).eps else 0.0
    return time_ratio > 10.0 or zscore > 10.0


def _robust_fit(A, y, rho=1, iters=1000):
    """Huber-loss line fit by ADMM, as in pyxdf."""
    A = np.copy(A)
    offset = np.min(A[:, 1])
    A[:, 1] -= offset
    Aty = np.dot(A.T, y)
    L = np.linalg.cholesky(np.dot(A.T, A))
    U = L.T
    z = np.zeros_like(y)
    u = z
    x = z
    for _ in range(iters):
        x = np.linalg.solve(U, (np.linalg.solve(L, Aty + np.dot(A.T, z - u))))
        d = np.dot(A, x) - y + u
        d_inv = np.zeros_like(d)
        np.divide(1, d, out=d_inv, where=d != 0)
        tmp = np.maximum(0, (1 - (1 + 1 / rho) * np.abs(d_inv)))
        z = rho / (1 + rho) * d + 1 / (1 + rho) * tmp * d
        u = d - z
    x[0] -= x[1] * offset
    return x


class Timebase:
    """Maps a stream's raw timestamps to the ones pyxdf.load_xdf returns."""

    def __init__(self, s, sync):
        self.sync = sync and s["count"] > 0 and bool(s["clock_times"])
        self.limit = None        # sample count after pyxdf drops a pylsl#67 extra sample
        self.ranges, self.coefs = [], []
        self.bounds = None       # first sample index of each clock range (several ranges)
        self.dejit = None        # (segment start indices, intercepts, slopes)
        times, values = list(s["clock_times"]), list(s["clock_values"])
        fc = s["footer_count"]
        if sync and fc is not None and s["count"] > fc and _last_offset_corrupted(times, values):
            times, values = times[:-1], values[:-1]
            self.limit = fc
        self.times = times
        if not self.sync or not times:
            self.sync = False
            return
        if len(times) > 1:
            resets = (np.diff(times) < 0) | (_clock_glitch(np.diff(times), 5, 5)
                                             & _clock_glitch(np.diff(values), 10, 1))
            self.ranges = _segment_indices(resets)
        else:
            self.ranges = [(0, 0)]
        for a, b in self.ranges:
            if a != b:
                X = np.column_stack([np.ones(b + 1 - a), np.array(times[a:b + 1]) / _WINSOR])
                y = np.array(values[a:b + 1]) / _WINSOR
                try:
                    c = _robust_fit(X, y)
                    c[0] *= _WINSOR
                except np.linalg.LinAlgError:
                    _warn(f"stream {s['id']}: clock offsets {(a, b)} cannot be used for sync")
                    c = [0, 0]
                self.coefs.append(c)
            else:
                self.coefs.append((values[a], 0))

    @property
    def needs_prepass(self):
        return self.sync and len(self.ranges) > 1

    def _range_of(self, raw, r=0):
        """Clock range of one sample under pyxdf's rule (closer to which range end)."""
        while r < len(self.ranges) - 1:
            stop = self.ranges[r][1] + 1
            if abs(raw - self.times[self.ranges[r][1]]) < abs(raw - self.times[stop]):
                return r
            r += 1
        return r

    def assign_ranges(self, raw):
        """pyxdf's split of all samples into clock ranges; needs every raw timestamp."""
        starts, ts_start = [], 0
        for r, (a, b) in enumerate(self.ranges):
            stop = b + 1
            if stop < len(self.times):
                cond = np.less(np.abs(raw[ts_start:] - self.times[b]),
                               np.abs(raw[ts_start:] - self.times[stop]))
                ts_stop = ts_start + len(cond) if all(cond) else ts_start + np.argmin(cond).item()
            else:
                ts_stop = len(raw)
            starts.append(ts_start if ts_start != ts_stop else -1)
            ts_start = ts_stop
        self.bounds = starts

    def clock(self, raw, idx):
        """Clock-corrected copy of raw timestamps; idx holds their sample indices."""
        if not self.sync:
            return raw
        if len(self.ranges) == 1:
            c = self.coefs[0]
            return raw + (c[0] + c[1] * raw)
        out = np.empty_like(raw)
        if self.bounds is None:
            # Only for single values such as the first sample; playback uses bounds.
            for i in range(len(raw)):
                c = self.coefs[self._range_of(raw[i])]
                out[i] = raw[i] + (c[0] + c[1] * raw[i])
            return out
        live = [(st, r) for r, st in enumerate(self.bounds) if st >= 0]
        seg = np.searchsorted([st for st, _ in live], idx, "right") - 1
        for u in np.unique(seg):
            m = seg == u
            c = self.coefs[live[u][1]]
            out[m] = raw[m] + (c[0] + c[1] * raw[m])
        return out

    def fit_dejitter(self, ts, s):
        """pyxdf's jitter removal: split at breaks, then a least-squares line per part."""
        sync = s["xml"].find("desc/synchronization/can_drop_samples")
        if s["srate"] == 0 or not len(ts) or (sync is not None and
                                              str(sync.text).lower() == "true"):
            return
        b_breaks = np.abs(np.diff(ts)) > np.max((1, 500 * s["tdiff"]))
        starts, m0, m1 = [], [], []
        for a, b in _segment_indices(b_breaks):
            idx = np.arange(a, b + 1, 1)[:, None]
            X = np.concatenate((np.ones_like(idx), idx), axis=1)
            mapping = np.linalg.lstsq(X, ts[idx], rcond=-1)[0]
            starts.append(a)
            m0.append(mapping[0, 0])
            m1.append(mapping[1, 0])
        self.dejit = (np.array(starts), np.array(m0), np.array(m1))

    def final(self, raw, idx):
        """Timestamps as pyxdf returns them; idx holds the samples' indices."""
        if self.dejit is None:
            return self.clock(raw, idx)
        starts, m0, m1 = self.dejit
        seg = np.searchsorted(starts, idx, "right") - 1
        return m0[seg] + m1[seg] * idx


# ---------------------------------------------------------------------------
# Samples chunk decoding
# ---------------------------------------------------------------------------
class Decoder:
    """Decodes Samples chunks of one stream, with pyxdf's deduced timestamps."""

    def __init__(self, s):
        self.fmt, self.nch, self.vs, self.tdiff = s["fmt"], s["nch"], s["vsize"], s["tdiff"]
        if self.vs:
            self.native = np.dtype(_NUMERIC_DTYPES[self.fmt])
            self.le = self.native.newbyteorder("<")
            self.rec = np.dtype([("tag", "u1"), ("ts", "<f8"), ("v", self.le, (self.nch,))])

    def decode(self, buf, last, values=True):
        """buf holds one Samples chunk after the stream id. Returns (ts, data, last).

        last is the stream's previous timestamp; a sample without its own stamp
        gets the previous one plus 1/srate, the same float operations as pyxdf.
        """
        n, p = _varlen_buf(buf, 0)
        if not self.vs:
            return self._strings(buf, p, n, last, values)
        if n == 0:
            return np.empty(0), np.empty((0, self.nch), self.native), last
        rest = len(buf) - p
        k, r = divmod(n * (9 + self.vs) - rest, 8)
        if r or not 0 <= k <= n:
            raise ValueError("sample sizes do not match the chunk length")
        if k == 0:
            rec = np.frombuffer(buf, self.rec, count=n, offset=p)
            if not np.all(rec["tag"]):
                raise ValueError("unstamped sample in a chunk sized for stamps only")
            ts = rec["ts"].astype(np.float64)
            data = np.ascontiguousarray(rec["v"], dtype=self.native) if values else None
            return ts, data, float(ts[-1])
        u8 = np.frombuffer(buf, np.uint8, count=rest, offset=p)
        f = self._flags(u8, buf, p, n, k)
        vpos = np.arange(n, dtype=np.int64) * (1 + self.vs) + 1 + 8 * np.cumsum(f)
        ts = np.empty(n)
        ex = np.flatnonzero(f)
        if len(ex):
            win8 = as_strided(u8, (rest - 7, 8), (1, 1))
            ts[ex] = win8[vpos[ex] - 8].view("<f8").ravel()
        self._fill_deduced(ts, f, last)
        data = None
        if values:
            winv = as_strided(u8, (rest - self.vs + 1, self.vs), (1, 1))
            data = np.ascontiguousarray(winv[vpos].view(self.le).reshape(n, self.nch),
                                        dtype=self.native)
        return ts, data, float(ts[-1])

    def _flags(self, u8, buf, p, n, k):
        """Which samples carry a timestamp. Each tag byte decides where the next
        one is, so this walks the tag bytes. Recorders store a stamp only where
        it differs from the deduced one, so there is no pattern to predict."""
        step = 1 + self.vs
        if k == n:
            f = np.zeros(n, bool)
            if np.any(u8[np.arange(n, dtype=np.int64) * step]):
                raise ValueError("timestamp flags do not match the chunk length")
            return f
        fl = bytearray(n)
        q = p
        for i in range(n):
            if buf[q]:
                fl[i] = 1
                q += step + 8
            else:
                q += step
        f = np.frombuffer(bytes(fl), dtype=bool)
        if n - int(f.sum()) != k:
            raise ValueError("timestamp flags do not match the chunk length")
        return f

    def _fill_deduced(self, ts, f, last):
        """Give each unstamped sample the previous timestamp plus 1/srate, added in
        sequence as pyxdf does, so long runs accumulate the same rounding."""
        n = len(f)
        idx = np.arange(n)
        run = idx - np.maximum.accumulate(np.where(f, idx, -1))   # 0 on stamped samples
        longest = int(run.max())
        if not longest:
            return
        ext = np.empty(n + 1)                 # ext[i + 1] is sample i; ext[0] is `last`
        ext[0] = last
        ext[1:] = ts
        # One vectorized step per position in a run fills all runs at once.
        order = np.argsort(run, kind="stable")
        cut = np.searchsorted(run[order], np.arange(1, min(longest, 32) + 2))
        for j in range(min(longest, 32)):
            sel = order[cut[j]:cut[j + 1]] + 1
            ext[sel] = ext[sel - 1] + self.tdiff
        if longest > 32:
            # Few runs are this long; np.add.accumulate adds in sequence too.
            stamped = np.flatnonzero(f)
            for i in np.flatnonzero(run == 33):
                k = np.searchsorted(stamped, i)
                e = stamped[k] if k < len(stamped) else n
                seq = np.full(e - i + 1, self.tdiff)
                seq[0] = ext[i]
                ext[i + 1:e + 1] = np.add.accumulate(seq)[1:]
        ts[:] = ext[1:]

    def _strings(self, buf, q, n, last, values):
        ts = np.empty(n)
        out = [] if values else None
        for i in range(n):
            if buf[q]:
                last = _F64.unpack_from(buf, q + 1)[0]
                q += 9
            else:
                last += self.tdiff
                q += 1
            ts[i] = last
            row = []
            for _ in range(self.nch):
                ln, q = _varlen_buf(buf, q)
                if values:
                    row.append(buf[q:q + ln].decode(errors="replace"))
                q += ln
            if values:
                out.append(row)
        return ts, out, last


def read_all_stamps(path, s, tb):
    """Every raw timestamp of one stream (for dejitter and clock-range splits)."""
    dec = Decoder(s)
    n_total = s["count"] if tb.limit is None else min(s["count"], tb.limit)
    out = np.empty(n_total)
    last = 0.0
    # Clock ranges are not assigned yet when this pass is what assigns them, and
    # clock() is only cheap once they are; such streams skip the check.
    cref = None if tb.needs_prepass else _chunk_starts(tb.clock(s["ref"], s["gstart"]),
                                                       s["gstart"], s["tdiff"])
    with _open(path)[0] as f:
        for c, (off, end, g0, n) in enumerate(zip(s["off"], s["end"], s["gstart"], s["n"])):
            if g0 >= n_total:
                break
            f.seek(off)
            try:
                ts, _, new_last = dec.decode(f.read(end - off), last, values=False)
                if cref is not None and not _plausible(tb.clock(ts, np.arange(g0, g0 + n)), cref, c):
                    raise ValueError("timestamps do not fit the chunks around it")
                last = new_last
            except (struct.error, IndexError, ValueError):
                # Playback skips this chunk; deduced stamps keep the fits finite.
                ts = last + s["tdiff"] * np.arange(1, n + 1)
                last = float(ts[-1]) if n else last
            m = min(len(ts), n_total - g0)
            out[g0:g0 + m] = ts[:m]
    return out


# ---------------------------------------------------------------------------
# Stream selection and listing
# ---------------------------------------------------------------------------
def first_time(s, tb):
    """Timestamp of the stream's first sample as pyxdf returns it. Dejitter
    applies only where it was fitted, that is, for streams being replayed."""
    if s["first"] is None:
        return None
    return float(tb.final(np.array([s["first"]]), np.zeros(1, np.int64))[0])


def print_list(streams, bases):
    firsts = {k: first_time(s, bases[k]) for k, s in streams.items()}
    t0 = min((v for v in firsts.values() if v is not None), default=0.0)
    print(f"{'id':>3}  {'name':<28} {'type':<14} {'ch':>4} {'rate':>9} {'format':<8}"
          f" {'samples':>10} {'start s':>9} {'dur s':>9}")
    for k, s in streams.items():
        rate = "irreg" if s["srate"] == 0 else f"{s['srate']:g}"
        if firsts[k] is None:
            begin = dur = "-"
        else:
            begin = f"{firsts[k] - t0:.2f}"
            last = bases[k].clock(np.array([s["last"] or s["first"]]), np.array([s["count"] - 1]))
            dur = f"{float(last[0]) - firsts[k]:.2f}"
        print(f"{s['id']:>3}  {s['name'][:28]:<28} {s['type'][:14]:<14} {s['nch']:>4}"
              f" {rate:>9} {s['fmt']:<8} {s['count']:>10} {begin:>9} {dur:>9}")
    print("'start' is relative to the earliest sample in the file, the time zero of --start.")


def select(streams, spec):
    if not spec:
        return list(streams)
    chosen = []
    for item in (x.strip() for x in spec.split(",")):
        if not item:
            continue
        if item.isdigit():
            hits = [int(item)] if int(item) in streams else []
        elif item.lower().startswith("type:"):
            pat = item[5:]
            hits = [k for k, s in streams.items() if fnmatch.fnmatchcase(s["type"], pat)]
        else:
            hits = [k for k, s in streams.items() if fnmatch.fnmatchcase(s["name"], item)]
        if not hits:
            raise SystemExit(f"no stream matches '{item}' (see --list)")
        chosen += [k for k in hits if k not in chosen]
    return chosen


# ---------------------------------------------------------------------------
# Outlet creation
# ---------------------------------------------------------------------------
def make_outlet(hdr, prefix, suffix, chunk_s):
    # Building the info from XML keeps the recorded <desc> verbatim, including
    # element order and attributes, which pylsl's XMLElement API cannot write.
    # The skeleton lists every field liblsl expects: it rejects a header with a
    # missing number or uid, and the outlet can only fill in its live
    # session_id, hostname, and ports where the elements already exist.
    root = ET.Element("info")
    for tag, text in (
        ("name", prefix + hdr["name"] + suffix), ("type", hdr["type"]),
        ("channel_count", str(hdr["nch"])), ("channel_format", hdr["fmt"]),
        ("source_id", hdr["source_id"]), ("nominal_srate", repr(hdr["srate"])),
        ("version", "1.1"), ("created_at", "0"), ("uid", "replay"),
        ("session_id", "default"), ("hostname", "replay"),
        ("v4address", ""), ("v4data_port", "0"), ("v4service_port", "0"),
        ("v6address", ""), ("v6data_port", "0"), ("v6service_port", "0"),
    ):
        ET.SubElement(root, tag).text = text
    desc = hdr["xml"].find("desc")
    root.append(desc if desc is not None else ET.Element("desc"))
    info = StreamInfo(handle=lib.lsl_streaminfo_from_xml(ET.tostring(root, encoding="utf-8")))
    if info.channel_count() != hdr["nch"]:
        raise RuntimeError(f"liblsl rejected the header of stream {hdr['id']}: {info.name()}")
    # A chunk_size hint lets liblsl batch regular data the same way the push loop does.
    srate = hdr["srate"]
    chunk = max(1, int(round(srate * chunk_s))) if srate > 0 else 0
    return StreamOutlet(info, chunk_size=chunk, max_buffered=360)


# ---------------------------------------------------------------------------
# Playback: one reader thread fills a queue per stream; one thread per outlet
# pushes from its queue on the shared clock.
# ---------------------------------------------------------------------------
class Track:
    def __init__(self, hdr, outlet, tb):
        self.hdr = hdr
        self.outlet = outlet
        self.tb = tb
        self.dec = Decoder(hdr)
        self.numeric = hdr["fmt"] != "string"
        # pylsl refuses int64 on Windows because its ctypes signatures use a
        # 32-bit c_long there. A raw pointer to an int64 buffer is correct on
        # every 64-bit platform, so call liblsl directly.
        self.push_fn = lib.lsl_push_chunk_ltnp if hdr["fmt"] == "int64" else outlet.do_push_chunk_n
        self.regular = hdr["srate"] > 0
        self.q = queue.Queue()   # (rel, sched, data) items, then None
        self.ref = np.empty(0)   # approximate start time of each chunk, set by plan()
        self.cref = np.empty(0)  # the same before dejitter, to check decoded stamps
        self.first_chunk = 0     # where the reader starts, set by plan()
        self.last0 = 0.0         # previous raw timestamp at that chunk (NaN if unknown)


def plan(track, lo):
    """Pick the chunk to start decoding at for a window that begins at lo."""
    s, tb = track.hdr, track.tb
    if not len(s["off"]):
        return
    track.cref = _chunk_starts(tb.clock(s["ref"], s["gstart"]), s["gstart"], s["tdiff"])
    ref = track.cref if tb.dejit is None else tb.final(s["ref"], s["gstart"])
    track.ref = ref
    ok = np.flatnonzero(np.isfinite(ref) & (ref <= lo - _MARGIN))
    c = int(ok[-1]) if len(ok) else 0
    if c and not s["first_exp"][c]:
        # The first sample's timestamp is deduced from the previous chunk, so
        # decode back to a chunk that carries a stamp of its own.
        c -= 1
        while c > 0 and not s["has_exp"][c]:
            c -= 1
    track.first_chunk = c
    track.last0 = 0.0 if c == 0 else np.nan


class Reader(threading.Thread):
    def __init__(self, path, tracks, lo, hi, speed, loop, lookahead, gap, base, stop):
        super().__init__(daemon=True)
        self.path, self.tracks, self.lo, self.hi = path, tracks, lo, hi
        self.speed, self.loop, self.lookahead, self.gap = speed, loop, lookahead, gap
        self.base, self.stop = base, stop
        self.late = 0
        self.empty = False
        self.f = None

    def run(self):
        try:
            self._run()
        except (struct.error, IndexError, ValueError) as e:
            _warn(f"damaged chunk ({e}), replay stops there")
        finally:
            if self.f is not None:
                self.f.close()
            for tr in self.tracks:
                tr.q.put(None)

    def _chunks(self, gz):
        """Yield (track index, chunk index), each stream's chunks in sequence.

        Plain files are read in order of chunk start time, so a stream whose
        next chunk lies far ahead (a sparse marker stream, or another host's
        clock without --sync) cannot hold up the others. Seeking backwards in
        gzip means decompressing from the start, so gzip goes in file order.
        """
        heap = []
        for i, tr in enumerate(self.tracks):
            if tr.first_chunk < len(tr.hdr["off"]):
                heap.append((self._key(tr, tr.first_chunk, -np.inf, gz), i))
        heapq.heapify(heap)
        cursor = [tr.first_chunk for tr in self.tracks]
        while heap:
            key, i = heapq.heappop(heap)
            c = cursor[i]
            if (yield i, c):
                continue     # the caller marked the stream done
            cursor[i] = c + 1
            if c + 1 < len(self.tracks[i].hdr["off"]):
                heapq.heappush(heap, (self._key(self.tracks[i], c + 1, key, gz), i))

    @staticmethod
    def _key(tr, c, prev, gz):
        if gz:
            return int(tr.hdr["off"][c])
        ref = tr.ref[c]
        return float(ref) if np.isfinite(ref) and ref > prev else prev

    def _run(self):
        lo, hi, speed = self.lo, self.hi, self.speed
        self.f, size = _open(self.path)
        gz = size is None
        k, loop_len = 0, 0.0
        while not self.stop.is_set():
            last = [tr.last0 for tr in self.tracks]
            limit = [tr.tb.limit if tr.tb.limit is not None else tr.hdr["count"] for tr in self.tracks]
            end_ts = -np.inf
            chunks = self._chunks(gz)
            done = None          # what the caller tells the generator: drop this stream?
            while True:
                try:
                    i, c = chunks.send(done)
                except StopIteration:
                    break
                done = True
                tr = self.tracks[i]
                s = tr.hdr
                g0 = int(s["gstart"][c])
                ref = tr.ref[c]
                if g0 >= limit[i] or (np.isfinite(ref) and ref > hi + _MARGIN):
                    continue
                if np.isfinite(ref):
                    due = self.base + (ref - lo) / speed + k * loop_len
                    while True:
                        ahead = due - local_clock() - self.lookahead
                        if ahead <= 0:
                            break
                        if self.stop.wait(min(ahead, 0.25)):
                            return
                off = int(s["off"][c])
                f = self.f
                if gz and off < f.tell():
                    f.close()   # gzip cannot seek backwards cheaply; start over
                    f = self.f = _open(self.path)[0]
                f.seek(off)
                try:
                    raw, data, new_last = tr.dec.decode(f.read(int(s["end"][c]) - off), last[i])
                    if not _plausible(tr.tb.clock(raw, np.arange(g0, g0 + len(raw))), tr.cref, c):
                        raise ValueError("timestamps do not fit the chunks around it")
                    last[i] = new_last
                except (struct.error, IndexError, ValueError) as e:
                    # The chunk length was intact (the scan indexed it) but the body is
                    # not; the rest of the stream is still good.
                    if k == 0:
                        _warn(f"{s['name']}: chunk at byte {off} cannot be decoded ({e}), skipped")
                    done = False
                    continue
                if g0 + len(raw) > limit[i]:
                    m = limit[i] - g0
                    raw, data = raw[:m], data[:m]
                if not len(raw):
                    done = False
                    continue
                ts = tr.tb.final(raw, np.arange(g0, g0 + len(raw)))
                done = bool(np.nanmin(ts) > hi + _MARGIN)
                keep = (ts >= lo) & (ts <= hi)
                if not keep.any():
                    continue
                if not keep.all():
                    ts = ts[keep]
                    data = ([data[x] for x in np.flatnonzero(keep)] if isinstance(data, list)
                            else data[keep])
                rel = (ts - lo) / speed
                if k:
                    rel += k * loop_len
                sched = rel if np.all(rel[1:] >= rel[:-1]) else np.maximum.accumulate(rel)
                if self.base + sched[0] < local_clock():
                    self.late += 1
                tr.q.put((rel, sched, data))
                if k == 0:
                    end_ts = max(end_ts, float(ts.max()))
            if k == 0:
                if end_ts == -np.inf:
                    self.empty = True
                    return
                # A gap of one period of the fastest stream keeps the wrap seamless
                # for a stream that spans the window and keeps every stream strictly
                # increasing.
                loop_len = (end_ts - lo + self.gap) / speed
            if not self.loop:
                return
            k += 1


def push_numeric(track, data, stamps):
    # pylsl's push_chunk unpacks a timestamp list one float at a time; liblsl
    # can read the numpy buffers directly, so call it without the copy.
    rc = track.push_fn(track.outlet.obj, ctypes.c_void_p(data.ctypes.data),
                       ctypes.c_ulong(data.size), ctypes.c_void_p(stamps.ctypes.data),
                       ctypes.c_int(1))
    if rc < 0:
        raise RuntimeError(f"liblsl push_chunk failed with error {rc}")


def play(track, base, chunk_s, stop):
    outlet = track.outlet
    buf = np.empty(max(16, int(track.hdr["srate"] * chunk_s * 4)), dtype=np.float64)
    item, i = None, 0
    while True:
        if item is None:
            try:
                item = track.q.get(timeout=0.2)
            except queue.Empty:
                if stop.is_set():
                    return
                continue
            if item is None:
                return           # the reader has finished
            i = 0
        rel, sched, data = item
        now = local_clock()
        j = int(sched.searchsorted(now - base, "right"))
        if j > i:
            m = j - i
            if m > len(buf):
                buf = np.empty(2 * m, dtype=np.float64)
            stamps = buf[:m]
            np.add(rel[i:j], base, out=stamps)
            if track.numeric:
                push_numeric(track, data[i:j], stamps)
            elif m == 1:
                outlet.push_sample(data[i], float(stamps[0]))
            else:
                outlet.push_chunk(data[i:j], stamps.tolist())
            i = j
        if i >= len(rel):
            # File chunks can be shorter than one push period, so go straight on
            # to the next queued chunk instead of waiting.
            item = None
            continue
        # Regular streams push about once per period; irregular streams wake up
        # at the time of their next sample.
        wait = sched[i] + base - now
        if track.regular:
            wait = max(wait, chunk_s)
        if stop.wait(wait):
            return


def main():
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("file", help="XDF file (.xdf or gzip-compressed .xdfz)")
    p.add_argument("--list", action="store_true",
                   help="print the streams in the file and exit (reads headers only)")
    p.add_argument("--streams", default="",
                   help="comma list of stream ids, name patterns, or type:<pattern> (default: all)")
    p.add_argument("--prefix", default="", help="prepended to every outlet name")
    p.add_argument("--suffix", default="",
                   help="appended to every outlet name (avoid clashes with live devices)")
    p.add_argument("--start", type=float, default=0.0,
                   help="window start in seconds after the first sample in the file (default: 0)")
    p.add_argument("--duration", type=float, default=None,
                   help="window length in seconds (default: to the end)")
    p.add_argument("--speed", type=float, default=1.0,
                   help="playback speed factor; timestamps are scaled too (default: 1)")
    p.add_argument("--loop", action="store_true",
                   help="repeat the window; timestamps keep increasing across passes")
    p.add_argument("--delay", type=float, default=0.5,
                   help="seconds between outlet creation and the first sample, so"
                        " inlets can connect (default: 0.5)")
    p.add_argument("--chunk-ms", type=float, default=20.0,
                   help="push period for regular streams in ms (default: 20)")
    p.add_argument("--lookahead", type=float, default=2.0,
                   help="seconds the reader decodes ahead of playback (default: 2)")
    p.add_argument("--sync", action=argparse.BooleanOptionalAction, default=True,
                   help="apply pyxdf's clock synchronization (default: on)")
    p.add_argument("--dejitter", action=argparse.BooleanOptionalAction, default=False,
                   help="apply pyxdf's timestamp dejittering (default: off, so the"
                        " replay keeps the recorded jitter)")
    args = p.parse_args()
    if args.speed <= 0:
        p.error("--speed must be positive")
    if args.duration is not None and args.duration <= 0:
        p.error("--duration must be positive")
    if args.lookahead <= 0:
        p.error("--lookahead must be positive")
    chunk_s = args.chunk_ms / 1000.0

    t = time.perf_counter()
    headers = scan_xdf(args.file)
    bases = {k: Timebase(s, args.sync) for k, s in headers.items()}
    if args.list:
        print_list(headers, bases)
        print(f"(scanned in {time.perf_counter() - t:.2f} s)")
        return
    ids = select(headers, args.streams)

    t_pre = time.perf_counter()
    pre = [k for k in ids if bases[k].needs_prepass or
           (args.dejitter and headers[k]["srate"] > 0 and headers[k]["count"])]
    for k in pre:
        raw = read_all_stamps(args.file, headers[k], bases[k])
        if bases[k].needs_prepass:
            bases[k].assign_ranges(raw)
        if args.dejitter:
            bases[k].fit_dejitter(bases[k].clock(raw, np.arange(len(raw))), headers[k])
        del raw
    if pre:
        print(f"read all timestamps of {len(pre)} streams in {time.perf_counter() - t_pre:.2f} s")

    # Time zero is the first sample in the file, also when it belongs to a stream
    # that is not replayed, so --start means the same as the --list column.
    # Dejitter can move a replayed stream's first sample earlier; time zero
    # follows it so that --start 0 keeps every sample.
    firsts = [v for v in (first_time(headers[k], bases[k]) for k in headers) if v is not None]
    if not firsts:
        raise SystemExit("the file contains no samples")
    t0 = min(firsts)
    lo = t0 + args.start
    hi = lo + args.duration if args.duration is not None else np.inf

    tracks = []
    print("outlets:")
    for k in ids:
        hdr = headers[k]
        tr = Track(hdr, make_outlet(hdr, args.prefix, args.suffix, chunk_s), bases[k])
        plan(tr, lo)
        tracks.append(tr)
        rate = "irregular" if hdr["srate"] == 0 else f"{hdr['srate']:g} Hz"
        note = "  (no samples: outlet stays idle)" if not hdr["count"] else ""
        print(f"  {args.prefix + hdr['name'] + args.suffix:<32} {hdr['type']:<12}"
              f" {hdr['nch']:>3} ch  {rate:<10} {hdr['fmt']:<8} {hdr['count']:>9} samples{note}")
    rates = [tr.hdr["srate"] for tr in tracks if tr.hdr["srate"] > 0]
    gap = 1.0 / max(rates) if rates else 1e-3

    stop = threading.Event()
    base = local_clock() + args.delay
    reader = Reader(args.file, [tr for tr in tracks if len(tr.hdr["off"])], lo, hi, args.speed,
                    args.loop, args.lookahead, gap, base, stop)
    for tr in tracks:
        if not len(tr.hdr["off"]):
            tr.q.put(None)
    reader.start()
    threads = [threading.Thread(target=play, args=(tr, base, chunk_s, stop), daemon=True)
               for tr in tracks]
    for th in threads:
        th.start()
    print(f"ready in {time.perf_counter() - t:.2f} s; playing from {args.start:g} s"
          f"{f' for {args.duration:g} s' if args.duration else ''}"
          f"{' in a loop' if args.loop else ''} at {args.speed:g}x. Press Ctrl+C to stop.",
          flush=True)
    try:
        while any(th.is_alive() for th in threads):
            for th in threads:
                th.join(0.2)
        if reader.empty:
            print("no samples in the selected window (see --list for stream times)")
        else:
            # Give connected inlets time to drain the last chunks before the outlets close.
            time.sleep(1.0)
            print(f"done. {reader.late} late chunks.")
    except KeyboardInterrupt:
        print(f"\nstopping... {reader.late} late chunks so far.")
        stop.set()
        for th in threads + [reader]:
            th.join(1.0)


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        # Ctrl+C before playback starts (scan, dejitter pre-pass) or while exiting.
        print("\nstopped.")
