#!/usr/bin/env python3
# =====================================================================
# feature_selection.py -- which vibration features earn their place in
# the node, and which FFT parameters fit this rig.
#
# The deployed vib profile is 6 features from a 128-point FFT at
# 200 Hz (vibration_sensor.h:55-56): rms, b1..b4 band powers, fmax;
# 0.64 s window, Hann, 1.5625 Hz bins. Two questions this tool answers
# from RAW rig data (raw x/y/z samples -- the 6 computed features alone
# cannot test window length or band edges):
#
#   1. FFT parameters: window length (64..512 points), sampling rate
#      (200 vs decimated 100 Hz), window function (hann vs rect).
#      Each setting is scored with the node's OWN decision rule, not a
#      generic classifier.
#   2. Features: a 22-candidate bank (time + frequency domain) ranked
#      for condition separability in the same anchored log2 space the
#      core uses, then a greedy top-K subset (default K=4).
#   Plus: a data-driven band-edge proposal from the per-bin separability
#   profile.
#
# METHOD (node proxy -- mirrors nodes/core):
#   anchors   A_f = max(6*sd_cal, SCORE_FLOOR), mu_f = cal mean
#             (feature_space.h arm(); SCORE_FLOOR node_config.h:39-43)
#   rel(x)    = log2((x+A)/(|mu|+A)), clamp +-12 decades
#             (feature_space.h rel(); REL_CLAMP node_config.h:114)
#   units     u(f) = std of a condition's rel values, floor 0.05
#             (per-condition radii, 2026-11-15; arm() unit floor)
#   match     argmin over conditions of max_f |rel-c(f)|/u(f)
#             (max-component matching, node_config.h:106-110; MATCH_Q
#             4.0 is the "inside radius" bound -- argmin needs no q)
#   score     leave-one-window-out classification accuracy
# Within-condition rel std is reported separately: the settle gate
# vetoes on ANY noisy feature (RESEARCH.md, Settle gate v2 row) and
# "vib fmax/bands" were the documented offenders there.
#
# INPUT (raw 3-axis accelerometer samples in g, at the node's 200 Hz
# beat; pass --fs-in if you recorded at another rate):
#   a) data/raw/<condition>.csv, one file per condition; for repeat
#      recordings of one condition use a subdirectory per condition
#      (data/raw/<condition>/*.csv). Columns x,y,z in any order; one
#      optional time column; header optional. Condition names are exact
#      file/subdirectory names (speed_1 stays speed_1).
#   b) one CSV with a label/condition column naming every row.
#   c) --capture: record a) from the node's raw stream (console 'R',
#      vib node): the tool toggles 'R' around each condition so no stale
#      samples leak between recordings, and writes the CSVs itself.
#      --reset wipes the model first so the node stays IDLE (pure
#      telemetry, like collect_data.py --idle).
#
# RECORD ~30 s per condition (the tool default). A steady machine gives
# repetitive windows, so length buys little; the study flags any
# condition too thin for the 512-point row. Windows are non-overlapping,
# exactly like the firmware.
#
# USAGE
#   python3 tools/feature_selection.py --capture             # record first
#   python3 tools/feature_selection.py --capture --reset     # wipe + record
#   python3 tools/feature_selection.py --capture --dur 60 \
#       --campaign speed_2                                   # one condition
#   python3 tools/feature_selection.py --check <capture.csv> # quality gate
#   python3 tools/feature_selection.py                       # study data/raw/
#   python3 tools/feature_selection.py data/raw_run2         # other dir
#   python3 tools/feature_selection.py --cal speed_1         # different CAL
#   python3 tools/feature_selection.py --fft-ns 128,256 --fs-list 200
#   python3 tools/feature_selection.py --no-plot             # numpy only
#
# --check runs the DEPLOYED DSP (DC-block @ 200 Hz, 2:1 decimation, 64-pt
# Hann @ 100 Hz) on ONE capture and answers: is the 200 Hz stream intact
# (rate, per-second counts, line gaps), is the spectrum clean (top peaks,
# entropy), are the deployed 4 features stationary over the capture.
# Verdict lines use OK/!! like the capture's implied-rate warnings.
#
# OUTPUT: data/analysis/feature_selection_<ts>/
#   report.md  window_study.csv  features_ranked.csv  subset_path.csv
#   spectra.png  window_tradeoff.png  feature_scores.png  (no --no-plot)
# =====================================================================
import argparse
import csv
import os
import re
import sys
import time

import numpy as np

# ---- deployed constants (anchors to source) -------------------------
FS_IN_DEF    = 200.0   # vibration_sensor.h TICK_US 5000 (200 Hz beat)
HP_FC        = 0.5     # vibration_sensor.h:57 DC blocker fc ~ 0.5 Hz
FFT_NS_DEF   = "64,128,256,512"   # 128 = deployed (vibration_sensor.h:55)
FS_LIST_DEF  = "200,100"
WINFUNCS_DEF = "hann,rect"
BASELINE_N   = 128     # the deployed setting, always shown in the study
BANDS        = [("b1", 1.5625, 9.0), ("b2", 9.0, 24.0),
                ("b3", 24.0, 55.0), ("b4", 55.0, 99.0)]
# band edges reproduce vibration_sensor.cpp:97-109 exactly at n=128:
# b1 bins 1-5, b2 6-15, b3 16-35, b4 36-63 (Nyquist bin excluded, like
# the firmware loop k=1..FFT_N/2-1)
MATCH_Q      = 4.0     # node_config.h:116 (reported, not used by argmin)
REL_CLAMP    = 12.0    # node_config.h:114
SCORE_FLOOR  = 1e-4    # node_config.h:42
ANCHOR_K     = 6.0     # feature_space.h arm(): anchor = 6 sigma
UNIT_FLOOR   = 0.05    # feature_space.h arm(): unit floor
TELEMETRY_S  = 2.0     # [RAW] cadence (collect_data.py), latency budget
MIN_WIN      = 6       # min windows per condition for a valid config

BANK_TIME = ["rms", "rms_x", "rms_y", "rms_z", "peak", "p2p", "crest",
             "kurt", "skew", "impulse", "shape", "margin"]
BANK_FREQ_EXTRA = ["fmax", "centroid", "spread", "rolloff85", "flatness",
                   "entropy", "ratio_hi"]
BANK_ALL = BANK_TIME + [b[0] for b in BANDS] + BANK_FREQ_EXTRA   # 22

DEFAULT_CAMPAIGN = ["op_stopped", "speed_1", "speed_2",
                    "speed_1_phase_loss", "speed_2_phase_loss",
                    "speed_1_imbalance", "speed_2_imbalance"]
# imbalance is vib-only: the drive's electrical features (fset/v/i/pf)
# cannot see a mechanical imbalance, so the MODBUS campaign omits it
# (user decision, 2026-11-21).

# serial raw-sample line: `x,y,z` or `R x,y,z`, floats in g
SAMPLE_RE = re.compile(r"^\s*(?:R\s+)?\s*([-+0-9.eE]+)\s*,"
                       r"\s*([-+0-9.eE]+)\s*,\s*([-+0-9.eE]+)\s*$")

FS_IN = FS_IN_DEF            # module config, set by --fs-in in main()
CAL = None                   # module config, set by --cal in main()


# =====================================================================
# 1. capture mode
# =====================================================================
def _wait_ack(fd, pred, timeout, what):
    """Read serial lines until pred(line) or timeout. Consumed lines are
    stream chatter (stale samples between toggles), never recorded data."""
    t0 = time.monotonic()
    buf = b""
    while time.monotonic() - t0 < timeout:
        try:
            buf += os.read(fd, 4096)
        except (BlockingIOError, OSError):
            import select
            select.select([fd], [], [], 0.05)
            continue
        while b"\n" in buf:
            raw, buf = buf.split(b"\n", 1)
            if pred(raw.decode(errors="replace").strip()):
                return True
    print(f"!!! no {what} ack - continuing (check the log/stream)")
    return False


def capture(port, dur, outroot, reset, labels=None):
    """Record per-condition raw sample CSVs via the node's 'R' stream."""
    labels = labels or DEFAULT_CAMPAIGN
    from rig_tour import find_port, open_port, prompt   # tools/ shared
    if port in (None, "auto"):
        port = find_port()
    fd = open_port(port)
    print(f"== capture: {port} -> {outroot}/  ({dur:g} s per condition: "
          f"{', '.join(labels)})")
    os.makedirs(outroot, exist_ok=True)
    if reset:
        print("--- sending reset: node IDLE, pure telemetry")
        os.write(fd, b"reset\n")
        _wait_ack(fd, lambda l: "[FS] back to IDLE" in l, 10, "reset")
    for label in labels:
        prompt(f"Set the machine to {label}. Confirm when ready")
        os.write(fd, b"R\n")     # stream ON: ack-wait drops the stale tail
        if not _wait_ack(fd, lambda l: l.startswith("[VIB] raw")
                         and "ON" in l, 5, "raw-stream ON"):
            print("    (older firmware? the tool still records any "
                  "bare 'x,y,z' lines it sees)")
        path = os.path.join(outroot, f"{label}.csv")
        n = _capture_one(fd, dur, path)
        os.write(fd, b"R\n")     # OFF: no stale samples between conditions
        _wait_ack(fd, lambda l: l.startswith("[VIB] raw")
                  and "OFF" in l, 5, "raw-stream OFF")
        rate = n / dur
        print(f"--- {label}: {n} samples ({rate:.0f} Hz implied)")
        if rate < 0.8 * FS_IN:
            print(f"!!! implied rate {rate:.0f} Hz is well below "
                  f"{FS_IN:.0f} -- the stream is lossy or the lines do "
                  "not match 'x,y,z'; check the format")
        elif rate > 1.2 * FS_IN:
            print(f"!!! implied rate {rate:.0f} Hz is ABOVE "
                  f"{FS_IN:.0f} -- the node's sample beat is broken "
                  "(old firmware? reflash) -- windows would be far too "
                  "short, do not study this data")
    print("== capture done. Now run: "
          f"python3 tools/feature_selection.py {outroot}")


def _capture_one(fd, dur, path):
    t0 = time.monotonic()
    n = 0
    buf = b""
    last = -2.0
    with open(path, "w") as f:
        f.write("t_s,x,y,z\n")
        while time.monotonic() - t0 < dur:
            try:
                buf += os.read(fd, 4096)
            except (BlockingIOError, OSError):
                import select
                select.select([fd], [], [], 0.05)
                continue
            while b"\n" in buf:
                raw, buf = buf.split(b"\n", 1)
                m = SAMPLE_RE.match(raw.decode(errors="replace"))
                if m:
                    n += 1
                    f.write(f"{time.monotonic() - t0:.6f},"
                            f"{m.group(1)},{m.group(2)},{m.group(3)}\n")
            el = time.monotonic() - t0
            if el >= last + 2:
                last = el
                print(f"   ... {n} samples (T+{el:.0f}s)", flush=True)
    return n


# =====================================================================
# 1c. --check: single-capture quality gate (rate + spectrum + features)
# =====================================================================
def _load_check(path):
    """One capture CSV -> (t_s | None, (3, m) samples). Understands the
    capture format (t_s,x,y,z + header) and bare x,y,z (no clock: the
    rate check then assumes --fs-in and the gap scan is skipped)."""
    with open(path) as f:
        rows = [r for r in csv.reader(f) if r and any(t.strip() for t in r)]
    if not rows:
        sys.exit(f"!! {path}: empty")
    toks = [t.strip().lower() for t in rows[0]]
    headered = not all(_is_float(t) for t in toks if t != "")
    data = rows[1:] if headered else rows
    ncols = max(len(r) for r in data)
    if headered:
        cols = {t: i for i, t in enumerate(toks) if t}
        if not {"x", "y", "z"} <= cols.keys():
            sys.exit(f"!! {path}: header {toks} lacks x/y/z columns")
        ti = cols.get("t_s", cols.get("t"))
        idx = (ti, cols["x"], cols["y"], cols["z"])
    else:
        idx = tuple(range(ncols - 4, ncols)) if ncols >= 4 \
            else (None, 0, 1, 2)
    t, xyz = [], [[], [], []]
    for r in data:
        try:
            vals = [float(r[i]) if i is not None else None for i in idx]
        except (ValueError, IndexError):
            sys.exit(f"!! {path}: non-numeric row {r}")
        t.append(vals[0])
        for a in range(3):
            xyz[a].append(vals[a + 1])
    sig = np.asarray(xyz)
    if sig.shape[1] < 10 * FS_IN:
        print(f"!!! only {sig.shape[1]} samples -- under 10 s at "
              f"{FS_IN:g} Hz; the checks below run anyway")
    return (np.asarray(t) if t and t[0] is not None else None), sig


def check_capture(path, n_fft=64, fs_eff=100.0, win="hann", plot=True):
    """One raw capture -> is the 200 Hz stream intact, is the spectrum
    usable, are the deployed 4 features stationary. The DSP path mirrors
    vibration_sensor.cpp exactly: DC-block at FS_IN, 2:1 decimation to
    fs_eff, n_fft-point non-overlapping windows."""
    t, sig = _load_check(path)
    m = sig.shape[1]
    print(f"== check {path}: {m} samples")

    # ---- 1. stream integrity (host-side view) ------------------------
    if t is not None:
        dur = t[-1] - t[0]
        hz = (m - 1) / dur if dur > 0 else float("nan")
        gaps = np.diff(t)
        med_gap = float(np.median(gaps)) * 1e3
        max_gap = float(gaps.max())
        per_s = np.bincount(np.floor(t - t[0]).astype(int),
                            minlength=int(np.ceil(dur)) + 1)
        # first/last buckets are partial seconds by construction; only an
        # interior dip is a real stall
        core = per_s[1:-1] if len(per_s) >= 3 else per_s
        worst = int(core.min())
        ok = 0.95 * FS_IN <= hz <= 1.05 * FS_IN
        print(f"   rate: {hz:6.1f} Hz implied "
              f"({dur:.1f} s span) {'OK' if ok else '!! OUT OF BAND'} "
              f"| median gap {med_gap:.2f} ms")
        print(f"   per-second count (interior): min {worst} med "
              f"{int(np.median(core))} "
              f"{'OK' if worst >= 0.9 * FS_IN else '!! DIP (a stall)'}")
        print(f"   longest line gap: {max_gap * 1e3:.0f} ms "
              f"{'OK' if max_gap <= 0.5 else '!! STREAM STALL'}")
        if hz > 1.05 * FS_IN:
            print("!!! rate ABOVE fs-in -- the sample-beat burst pattern "
                  "(see 2026-11-21 capture): do not use this data")
    else:
        dur = m / FS_IN
        print("   (no time column: duration assumed from --fs-in, "
              "gap scan skipped)")

    # ---- 2. the deployed DSP path ------------------------------------
    d = decimate2(dc_block(sig, FS_IN))
    frames = make_frames(d, n_fft)
    k = frames.shape[1]
    if k < 10:
        sys.exit(f"!! only {k} full windows at n={n_fft} @ {fs_eff:g} Hz "
                 f"-- record longer")
    freqs = np.arange(1, n_fft // 2) * fs_eff / n_fft
    p = power_spec(frames, win)
    fmask = {name: (freqs >= lo) & (freqs < hi) for name, lo, hi in BANDS}
    ff = freq_features(p, freqs, fmask)
    tf = time_features(frames)
    series = [("rms_y", tf["rms_y"]), ("entropy", ff["entropy"]),
              ("margin", tf["margin"]), ("rms_x", tf["rms_x"])]

    # ---- 3. deployed-feature stationarity (the decision path) --------
    print(f"   windows: {k} x {n_fft} @ {fs_eff:g} Hz "
          f"({k * n_fft / fs_eff:.1f} s analysed)")
    unstable = []
    for name, s in series:
        s = s[~np.isnan(s)]
        mu, sd = s.mean(), s.std()
        cv = sd / max(abs(mu), 1e-30)
        half = len(s) // 2
        drift = abs(s[half:].mean() / max(s[:half].mean(), 1e-30) - 1)
        flag = "!!" if cv > 0.15 or drift > 0.20 else "OK"
        if flag == "!!":
            unstable.append(name)
        print(f"   {name:8s} mean {mu:11.4g} cv {cv:5.3f} "
              f"drift {drift * 100:5.1f}%  {flag}")
    if unstable:
        print(f"!!! non-stationary: {', '.join(unstable)} -- CAL/bands "
              "will form wide; check machine steadiness or sensor "
              "mounting before trusting this capture")

    # ---- 4. spectrum verdict ------------------------------------------
    P = p.mean(axis=0)
    loc = np.where((P[1:-1] >= P[:-2]) & (P[1:-1] >= P[2:]))[0] + 1
    peaks = loc[np.argsort(P[loc])[::-1][:5]] if len(loc) else \
        np.argsort(P)[::-1][:5]
    ent = float(ff["entropy"].mean())
    shape = ("tonal" if ent < 0.6 else
             "mixed" if ent < 0.85 else "broadband")
    print(f"   spectrum: {shape} (mean entropy {ent:.2f}; firmware "
          f"sees ~0.94 stopped-noise, ~0.58 tonal on the old capture)")
    share = P.sum()
    print("   top peaks: " + ", ".join(
        f"{freqs[i]:5.2f} Hz ({P[i] / max(share, 1e-30) * 100:4.1f}%)"
        for i in peaks))
    hi = freqs >= fs_eff / 2 * 0.75
    hshare = P[hi].sum() / max(share, 1e-30)
    if hshare > 0.2:
        print(f"!!! {hshare * 100:.0f}% of power in the top quarter of the "
              "band (37-50 Hz) -- possible fold from 100-200 Hz (no "
              "anti-alias filter at 400 Hz ODR)")

    # ---- 5. figure -----------------------------------------------------
    if plot:
        try:
            import matplotlib
            matplotlib.use("Agg")
            import matplotlib.pyplot as plt
        except ImportError:
            print("   (matplotlib unavailable -- no figure)")
            return
        fig, ax = plt.subplots(2, 1, figsize=(9, 7))
        lo, hi_b = np.percentile(p, 10, axis=0), np.percentile(p, 90, axis=0)
        ax[0].fill_between(freqs, lo, hi_b, alpha=0.3, label="p10-p90")
        ax[0].semilogy(freqs, np.maximum(P, 1e-30), label="mean")
        for i in peaks:
            ax[0].annotate(f"{freqs[i]:.1f}", (freqs[i], P[i]),
                           fontsize=8, ha="center", va="bottom")
        ax[0].set_xlabel("Hz (deployed: 64-pt @ 100 Hz, Hann)")
        ax[0].set_ylabel("power")
        ax[0].legend(fontsize=8)
        ax[0].set_title(os.path.basename(path))
        for name, s in series:
            ax[1].plot(s / max(abs(s.mean()), 1e-30), label=name)
        ax[1].axhline(1.15, ls=":", c="k", lw=0.5)
        ax[1].axhline(0.85, ls=":", c="k", lw=0.5)
        ax[1].set_xlabel("window (0.64 s each)")
        ax[1].set_ylabel("feature / mean")
        ax[1].legend(fontsize=8)
        fig.tight_layout()
        out = os.path.splitext(path)[0] + "_check.png"
        fig.savefig(out, dpi=120)
        print(f"   figure: {out}")


# =====================================================================
# 2. loading
# =====================================================================
def load_raw(input_path):
    """Return {condition: (3, m) float64 raw samples} from a directory
    or a single CSV. Labeled files are split by their label column."""
    if not os.path.exists(input_path):
        sys.exit(f"!! input not found: {input_path}")
    files = []                              # (path, forced condition | None)
    if os.path.isfile(input_path):
        files.append((input_path, None))
    else:
        top = [f for f in sorted(os.listdir(input_path))
               if f.lower().endswith(".csv")]
        files += [(os.path.join(input_path, f), None) for f in top]
        for sub in sorted(os.listdir(input_path)):
            d = os.path.join(input_path, sub)
            if not os.path.isdir(d):
                continue
            for f in sorted(os.listdir(d)):
                if f.lower().endswith(".csv"):
                    files.append((os.path.join(d, f), sub))
    if not files:
        sys.exit(f"!! no CSV files under {input_path}")
    conds = {}
    for path, forced in files:
        for label, arr in _load_csv(path, forced):
            conds.setdefault(label, []).append(arr)
    return {label: np.concatenate(parts, axis=1)
            for label, parts in sorted(conds.items())}


def _load_csv(path, forced=None):
    """One file -> [(condition, (3, m) samples)]. Header optional; with
    a header, x/y/z are matched by name and an optional label/condition
    column splits the file into several conditions. `forced` overrides
    the condition name (subdirectory rule)."""
    with open(path) as f:
        rows = [r for r in csv.reader(f) if r and any(t.strip() for t in r)]
    if not rows:
        sys.exit(f"!! {path}: empty")
    toks = [t.strip().lower() for t in rows[0]]
    headered = not all(_is_float(t) for t in toks if t != "")
    data = rows[1:] if headered else rows
    stem = forced or os.path.splitext(os.path.basename(path))[0]
    cols = {t: i for i, t in enumerate(toks) if t} if headered else {}

    def xyz_idx():
        idx = [cols.get(a) for a in ("x", "y", "z")]
        if None in idx:
            sys.exit(f"!! {path}: header {toks} lacks x/y/z columns")
        return idx

    if headered:
        idx = xyz_idx()
        lab = next((c for c in ("label", "condition") if c in cols), None)
    else:
        idx = [-3, -2, -1]
        lab = None

    def samp(row):
        try:
            return [float(row[i]) for i in idx]
        except (ValueError, IndexError):
            sys.exit(f"!! {path}: non-numeric row {row}")

    if lab is None:
        vals = np.array([samp(r) for r in data]).T        # (3, m)
        return [(stem, vals)]
    groups = {}
    for r in data:
        groups.setdefault(r[cols[lab]].strip() or stem, []).append(samp(r))
    return [(k, np.array(v).T) for k, v in sorted(groups.items())]


def _is_float(s):
    try:
        float(s)
        return True
    except ValueError:
        return False


# =====================================================================
# 3. DSP primitives (firmware-faithful)
# =====================================================================
def dc_block(sig, fs):
    """Per-axis first-order DC tracker, firmware-style (seeds with the
    first sample). alpha reproduces vibration_sensor.h:57 (0.016 at
    200 Hz) for the same 0.5 Hz corner."""
    a = 1.0 - np.exp(-2.0 * np.pi * HP_FC / fs)
    out = np.empty_like(sig)
    dev = sig[:, 0].copy()
    out[:, 0] = 0.0
    for t in range(1, sig.shape[1]):
        x = sig[:, t]
        out[:, t] = x - dev
        dev += a * (x - dev)
    return out


def decimate2(sig):
    """One 2:1 stage: 2-tap boxcar lowpass then stride 2. Applied only
    for fs_list entries below --fs-in (200->100 keeps fc at 0.5 Hz)."""
    m = sig.shape[1] // 2
    return (sig[:, 0:2 * m:2] + sig[:, 1:2 * m:2]) / 2.0


def make_frames(sig, n):
    """Non-overlapping windows, exactly like the firmware fills wdev_.
    Returns (3, k, n); the tail shorter than n is dropped."""
    k = sig.shape[1] // n
    return sig[:, :k * n].reshape(3, k, n)


def power_spec(frames, win):
    """Summed 3-axis power spectrum, DC + Nyquist bins excluded (the
    firmware loops k=1..FFT_N/2-1). Periodic Hann matches the firmware
    formula. Returns (k, K) with K = n/2 - 1."""
    if win == "hann":
        w = np.hanning(frames.shape[2] + 1)[:-1]
    else:
        w = np.ones(frames.shape[2])
    x = frames.astype(np.float64) * w[None, None, :]
    spec = np.fft.rfft(x, axis=2)
    return (np.abs(spec) ** 2).sum(axis=0)[:, 1:-1]


# =====================================================================
# 4. feature bank (22 candidates; every one is O(n) or O(bins))
# =====================================================================
def time_features(d):
    """d: (3, k, n) DC-blocked window frames -> {name: (k,) array}.
    wvib is the firmware's per-sample vector magnitude."""
    wvib = np.sqrt((d ** 2).sum(axis=0))                  # (k, n)
    mean_w = wvib.mean(axis=1)
    rms = np.sqrt((wvib ** 2).mean(axis=1))
    peak = wvib.max(axis=1)
    mu = d.mean(axis=2, keepdims=True)
    c = d - mu
    m2 = (c ** 2).mean(axis=2)
    m3 = (c ** 3).mean(axis=2)
    m4 = (c ** 4).mean(axis=2)
    sd = np.sqrt(np.maximum(m2, 1e-30))
    return {
        "rms": rms,
        "rms_x": np.sqrt((d[0] ** 2).mean(axis=1)),
        "rms_y": np.sqrt((d[1] ** 2).mean(axis=1)),
        "rms_z": np.sqrt((d[2] ** 2).mean(axis=1)),
        "peak": peak,
        "p2p": wvib.max(axis=1) - wvib.min(axis=1),
        "crest": peak / np.maximum(rms, 1e-30),
        "kurt": (m4 / np.maximum(m2 ** 2, 1e-30) - 3.0).mean(axis=0),
        "skew": (m3 / np.maximum(sd ** 3, 1e-30)).mean(axis=0),
        "impulse": peak / np.maximum(mean_w, 1e-30),
        "shape": rms / np.maximum(mean_w, 1e-30),
        "margin": peak / np.maximum(mean_w ** 2, 1e-30),
    }


def freq_features(p, freqs, band_masks):
    """p: (k, K) summed power spectrum -> {name: (k,) array}. A band
    with no bins at this (n, fs) yields NaN and is dropped later."""
    with np.errstate(divide="ignore", invalid="ignore"):
        tot = p.sum(axis=1)
        cent = (p * freqs[None, :]).sum(axis=1) / np.maximum(tot, 1e-30)
        spread = np.sqrt(
            (p * (freqs[None, :] - cent[:, None]) ** 2).sum(axis=1)
            / np.maximum(tot, 1e-30))
        csum = np.cumsum(p, axis=1)
        roll_i = (csum >= 0.85 * csum[:, -1:]).argmax(axis=1)
        eps = 1e-12 * np.maximum(p.max(axis=1, keepdims=True), 1e-30)
        flat = np.exp(np.log(p + eps).mean(axis=1)) / np.maximum(
            p.mean(axis=1), 1e-30)
        pr = p / np.maximum(tot[:, None], 1e-30)
        pl = np.where(pr > 0, pr * np.log(np.maximum(pr, 1e-30)), 0.0)
        # all-zero window: firmware reports max entropy (pure noise)
        ent = np.where(tot > 0,
                       -pl.sum(axis=1) / np.log(len(freqs)), 1.0)
    out = {name: (p[:, m].mean(axis=1) if m.any()
                  else np.full(len(p), np.nan))
           for name, m in band_masks.items()}
    out.update({
        "fmax": freqs[p.argmax(axis=1)],
        "centroid": cent,
        "spread": spread,
        "rolloff85": freqs[roll_i],
        "flatness": flat,
        "entropy": ent,
        "ratio_hi": (out["b3"] + out["b4"])
                    / np.maximum(out["b1"] + out["b2"], 1e-30),
    })
    return out


def compute_features(raw, n, fs_eff, win="hann"):
    """One config: raw dict -> (names, {cond: (k, F)}, {cond: (p, freqs)}).
    DC-block once at --fs-in (firmware order), then decimate for lower
    analysis rates."""
    out, spectra = {}, {}
    for cond, sig in raw.items():
        d = dc_block(sig, FS_IN)
        f = FS_IN
        while f > fs_eff * 1.5:
            d = decimate2(d)
            f /= 2.0
        fr = make_frames(d, n)
        if fr.shape[1] == 0:
            out[cond] = np.full((0, len(BANK_ALL)), np.nan)
            continue
        tf = time_features(fr)
        p = power_spec(fr, win)
        freqs = np.arange(1, p.shape[1] + 1) * f / n
        fmask = {name: (freqs >= lo) & (freqs < hi)
                 for name, lo, hi in BANDS}
        ff = freq_features(p, freqs, fmask)
        feats = {**tf, **ff}
        out[cond] = np.column_stack([feats[f] for f in BANK_ALL])
        spectra[cond] = (p, freqs)
    return BANK_ALL, out, spectra


# =====================================================================
# 5. node proxy: anchored rel space + leave-one-window-out match
# =====================================================================
def build_rel(cond_feats, cal):
    """Map every condition's feature matrix into the anchored log2
    space, mirroring FeatureSpace::arm() + rel() (feature_space.h)."""
    X = cond_feats[cal]
    mu = np.nanmean(X, axis=0)
    sd = np.nanstd(X, axis=0)
    anchor = np.maximum(ANCHOR_K * sd, SCORE_FLOOR)
    denom = np.abs(mu) + anchor
    rel = {}
    for cond, M in cond_feats.items():
        num = np.maximum(M + anchor[None, :], (denom * 1e-3)[None, :])
        rel[cond] = np.clip(np.log2(num / denom[None, :]),
                            -REL_CLAMP, REL_CLAMP)
    return rel, mu, anchor


def loo_eval(rel, S):
    """Leave-one-window-out accuracy of the node proxy on feature subset
    S. Own-condition centroid/units are recomputed without the test
    window (batch of n-1, like the node's 15-sample batches). Returns
    (acc, confusion, mean margin) or None if any condition is too thin."""
    conds = sorted(rel)
    X = {c: rel[c][:, S] for c in conds}
    if any(len(X[c]) < 3 for c in conds):
        return None
    CENT, U = {}, {}
    for c in conds:
        CENT[c] = X[c].mean(axis=0)
        U[c] = np.sqrt(np.maximum(X[c].var(axis=0), UNIT_FLOOR ** 2))
    truths, preds, margins = [], [], []
    for c in conds:
        xc = X[c]
        n_c = len(xc)
        ssum = xc.sum(axis=0)
        ssq = (xc ** 2).sum(axis=0)
        cent_wo = (ssum - xc) / (n_c - 1)                 # (n_c, F)
        var_wo = (ssq - xc ** 2) / (n_c - 1) - cent_wo ** 2
        u_wo = np.sqrt(np.maximum(var_wo, UNIT_FLOOR ** 2))
        d_own = np.max(np.abs(xc - cent_wo) / u_wo, axis=1)       # (n_c,)
        d_other = [np.max(np.abs(xc - CENT[k][None, :])
                          / U[k][None, :], axis=1)       # (n_c,)
                   for k in conds if k != c]
        d_mat = np.column_stack(d_other + [d_own[:, None]])
        idxs = [k for k in conds if k != c] + [c]
        local = d_mat.argmin(axis=1)
        srt = np.sort(d_mat, axis=1)
        margins.extend(srt[:, 1] - srt[:, 0])
        preds.extend(idxs[i] for i in local)
        truths.extend([c] * n_c)
    pos = {c: i for i, c in enumerate(conds)}
    conf = np.zeros((len(conds), len(conds)), dtype=int)
    for t, p in zip(truths, preds):
        conf[pos[t], pos[p]] += 1
    acc = float(np.mean(np.array(truths) == np.array(preds)))
    return acc, conf, float(np.mean(margins))


# =====================================================================
# 6. metrics + subset selection
# =====================================================================
def fisher_scores(rel):
    """Per-feature Fisher ratio in rel space: between-condition variance
    of centroids over mean within-condition variance. Returns
    (F, centroids (C,F), within-vars (C,F))."""
    conds = sorted(rel)
    C = np.stack([rel[c].mean(axis=0) for c in conds])
    W = np.stack([rel[c].var(axis=0) for c in conds])
    between = C.var(axis=0)
    within = W.mean(axis=0)
    f = np.where((within > 1e-12) & (between > 1e-12),
                 between / np.maximum(within, 1e-12), 0.0)
    f = np.where((within <= 1e-12) & (between > 1e-12), 1e6, f)
    return f, C, W


def worst_pair(C, W):
    """Min over condition pairs of centroid gap in pooled units: the
    most confusable pair per feature."""
    U = np.sqrt(np.maximum(W, 1e-12))
    vals = None
    for i in range(C.shape[0]):
        for j in range(i + 1, C.shape[0]):
            d = np.abs(C[i] - C[j]) / np.sqrt(U[i] ** 2 + U[j] ** 2)
            vals = d if vals is None else np.minimum(vals, d)
    return vals


def greedy_subset(rel, fisher, kmax):
    """Forward selection maximizing the node proxy's LOO accuracy
    (tie-break: summed Fisher of the subset). Returns the path
    [(feat_idx, acc, margin)]."""
    F = rel[sorted(rel)[0]].shape[1]
    S, path, remaining = [], [], list(range(F))
    while len(S) < kmax and remaining:
        best = None
        for f in remaining:
            ev = loo_eval(rel, S + [f])
            if ev is None:
                continue
            score = (ev[0], sum(fisher[g] for g in S + [f]))
            if best is None or score > best[0]:
                best = (score, f, ev)
        if best is None:
            break
        _, f, ev = best
        S.append(f)
        remaining.remove(f)
        path.append((f, ev[0], ev[2]))
    return S, path


# =====================================================================
# 7. window study
# =====================================================================
def cfg_label(n, fs, win):
    return f"n={n} fs={fs:g} {win}"


def window_study(raw, fft_ns, fs_list, winfuncs, min_win):
    """Score every (n, fs, win) config with the node proxy over the full
    bank. Returns (rows, keep) where keep[label] = (names, feats,
    spectra) for valid configs."""
    rows, keep = [], {}
    for n in fft_ns:
        for fs in fs_list:
            scale = FS_IN / fs
            if scale < 0.999 or abs(scale - round(scale)) > 1e-9 \
                    or (scale > 1 and round(scale) % 2 != 0):
                print(f"   skip fs={fs:g} (not a 2:1 decimation of "
                      f"{FS_IN:g} Hz)")
                continue
            scale = int(round(scale))
            for win in winfuncs:
                label = cfg_label(n, fs, win)
                kmin = min(sig.shape[1] // scale // n
                           for sig in raw.values())
                if kmin < min_win:
                    rows.append({"config": label, "acc": np.nan,
                                 "note": f"skip: only {kmin} windows "
                                         f"(min {min_win})"})
                    continue
                names, feats, spectra = compute_features(raw, n, fs, win)
                allM = np.vstack([v for v in feats.values() if len(v)])
                ok = ~np.isnan(allM).any(axis=0)
                if not ok.any():
                    rows.append({"config": label, "acc": np.nan,
                                 "note": "no usable features"})
                    continue
                sub = {c: v[:, ok] for c, v in feats.items()}
                rel, _, _ = build_rel(sub, CAL)
                ev = loo_eval(rel, list(range(int(ok.sum()))))
                if ev is None:
                    rows.append({"config": label, "acc": np.nan,
                                 "note": "too few windows"})
                    continue
                fis, _, _ = fisher_scores(rel)
                path4 = greedy_subset(rel, fis, 4)[1]
                acc4 = path4[-1][1] if len(path4) >= 4 else np.nan
                wstd = np.stack([rel[c].std(axis=0) for c in sorted(rel)])
                rows.append({
                    "config": label, "acc": ev[0], "acc4": acc4,
                    "win_s": n / fs, "bin_hz": fs / n,
                    "latency_s": n / fs,
                    "mean_within": float(wstd.mean()),
                    "max_within": float(wstd.max()),
                    "dropped": "+".join(np.array(names)[~ok]) or "-",
                    "note": "",
                })
                keep[label] = (names, feats, spectra)
    return rows, keep


# =====================================================================
# 8. band proposal from the per-bin separability profile
# =====================================================================
def propose_bands(spectra):
    """Per-bin Fisher profile across conditions -> valley-split band
    edges (Hz). Data-driven proposal: validate on a second recording.
    Returns (edges | None, profile, freqs)."""
    conds = sorted(spectra)
    P = [spectra[c][0] for c in conds]
    freqs = spectra[conds[0]][1]
    means = np.stack([p.mean(axis=0) for p in P])
    within = np.stack([p.var(axis=0) for p in P]).mean(axis=0)
    between = means.var(axis=0)
    prof = np.where((within > 1e-30) & (between > 1e-30),
                    between / np.maximum(within, 1e-30), 0.0)
    prof = np.where((within <= 1e-30) & (between > 1e-30), 1e6, prof)
    sm = np.convolve(prof, np.ones(3) / 3, mode="same")
    med = np.median(sm)
    peaks = [i for i in range(1, len(freqs) - 1)
             if sm[i] > med and sm[i] >= sm[i - 1] and sm[i] >= sm[i + 1]]
    if len(peaks) > 6:
        peaks = sorted(sorted(peaks, key=lambda i: -sm[i])[:6])
    if len(peaks) < 2:
        return None, prof, freqs
    edges = [freqs[int((a + b) / 2)] for a, b in zip(peaks, peaks[1:])]
    return [freqs[0]] + edges + [freqs[-1]], prof, freqs


def band_fisher(spectra, edges):
    """Fisher of mean-power features over the given edges (Hz).
    Conditions have different window counts, so per-condition stats are
    reduced to scalars before any cross-condition arithmetic."""
    conds = sorted(spectra)
    freqs = spectra[conds[0]][1]
    vals = []
    for i in range(len(edges) - 1):
        m = (freqs >= edges[i]) & (freqs < edges[i + 1])
        if not m.any():
            vals.append(0.0)
            continue
        feats = [spectra[c][0][:, m].mean(axis=1) for c in conds]
        between = np.array([f.mean() for f in feats]).var()
        within = np.array([f.var() for f in feats]).mean()
        vals.append(between / within if within > 0 else
                    (1e6 if between > 0 else 0.0))
    return vals


# =====================================================================
# 9. report + plots
# =====================================================================
def fmt(x, nd=3):
    if x is None or (isinstance(x, float) and np.isnan(x)):
        return "-"
    return f"{x:.{nd}f}"


def write_outputs(outdir, args, raw, rows, rec_label, names, rel, fisher,
                  wp, path, topk, bands_prop, conf, conf_conds, spectra):
    os.makedirs(outdir, exist_ok=True)

    with open(os.path.join(outdir, "window_study.csv"), "w") as f:
        cols = ["config", "acc", "acc4", "win_s", "bin_hz", "latency_s",
                "mean_within", "max_within", "dropped", "note"]
        f.write(",".join(cols) + "\n")
        for r in rows:
            f.write(",".join(str(fmt(r.get(c)) if c != "config"
                                     and c != "note" and c != "dropped"
                                     else r.get(c, ""))
                             for c in cols) + "\n")

    with open(os.path.join(outdir, "features_ranked.csv"), "w") as f:
        f.write("feature,fisher,solo_acc,within_std_decades,"
                "worst_pair_units\n")
        for i, name in enumerate(names):
            solo = loo_eval(rel, [i])
            wstd = float(np.mean([rel[c][:, i].std() for c in sorted(rel)]))
            f.write(f"{name},{fisher[i]:.3f},{fmt(solo[0]) if solo else '-'},"
                    f"{wstd:.3f},{fmt(wp[i]) if wp is not None else '-'}\n")

    with open(os.path.join(outdir, "subset_path.csv"), "w") as f:
        f.write("step,feature,acc,margin\n")
        for s, (fi, acc, marg) in enumerate(path, 1):
            f.write(f"{s},{names[fi]},{acc:.4f},{marg:.3f}\n")

    rep = []
    rep.append("# Vibration feature + FFT parameter selection\n")
    rep.append(f"- input: {args.input}")
    rep.append(f"- CAL condition: {CAL} (anchors A = 6 sigma, "
               "feature_space.h arm())")
    rep.append(f"- date: {time.strftime('%Y-%m-%d %H:%M')}\n")
    rep.append("## Inventory\n")
    rep.append("| condition | samples | seconds | windows @ n=128 |")
    rep.append("|---|---|---|---|")
    for c, sig in raw.items():
        rep.append(f"| {c} | {sig.shape[1]} | {sig.shape[1] / FS_IN:.0f} "
                   f"| {sig.shape[1] // BASELINE_N} |")
    rep.append("\n## Window study (node-proxy LOO accuracy)\n")
    rep.append("| config | acc | acc@4 | window s | bin Hz | mean within "
               "(dec) | max within | note |")
    rep.append("|---|---|---|---|---|---|---|---|")
    for r in rows:
        rep.append(f"| {r['config']} | {fmt(r.get('acc'), 4)} "
                   f"| {fmt(r.get('acc4'), 4)} | {fmt(r.get('win_s'), 2)} "
                   f"| {fmt(r.get('bin_hz'))} "
                   f"| {fmt(r.get('mean_within'))} "
                   f"| {fmt(r.get('max_within'))} | {r.get('note', '')} |")
    rep.append(f"\nRecommendation: **{rec_label}** -- within 2% of the "
               "best accuracy, then shortest window (latency vs the 2 s "
               "[RAW] cadence, TELEMETRY_S), then the calmest feature "
               "vector (max within-cond rel std; the settle gate vetoes "
               "on ANY noisy feature).\n")
    rep.append("## Feature ranking (recommended config)\n")
    rep.append("Fisher = between-cond var / mean within-cond var of the "
               "anchored rel coordinates. within_std is that feature's "
               "condition yardstick in decades: large values flap the "
               "settle gate (RESEARCH.md Settle gate v2 row).\n")
    rep.append("| feature | Fisher | solo acc | within std (dec) |")
    rep.append("|---|---|---|---|")
    for i in np.argsort(-fisher):
        solo = loo_eval(rel, [int(i)])
        wstd = float(np.mean([rel[c][:, i].std() for c in sorted(rel)]))
        rep.append(f"| {names[i]} | {fisher[i]:.2f} | "
                   f"{fmt(solo[0], 4) if solo else '-'} | {wstd:.3f} |")
    rep.append("\n## Top-K subset (greedy, node proxy)\n")
    rep.append("| step | feature | LOO acc | margin (units) |")
    rep.append("|---|---|---|---|")
    for s, (fi, acc, marg) in enumerate(path, 1):
        mark = " <- topk" if s == topk else ""
        rep.append(f"| {s} | {names[fi]} | {acc:.4f} | {marg:.2f}{mark} |")
    top_set = [names[fi] for fi, _, _ in path[:topk]]
    rep.append(f"\n**Selected top {topk}: {', '.join(top_set)}**\n")
    rep.append("Confusion at the top subset (rows = true condition):\n")
    rep.append("| " + " | ".join(conf_conds) + " |")
    rep.append("|" + "---|" * len(conf_conds))
    for c, row in zip(conf_conds, conf):
        rep.append(f"| {c} | " + " | ".join(str(v) for v in row) + " |")
    if bands_prop and bands_prop[0]:
        edges = bands_prop[0]
        rep.append("\n## Band-edge proposal (data-driven)\n")
        rep.append("proposed edges (Hz): "
                   + ", ".join(f"{e:.1f}" for e in edges))
        rep.append("proposed band Fisher: "
                   + ", ".join(f"{v:.1f}" for v in band_fisher(spectra, edges)))
        dbf = band_fisher(spectra, [b[1] for b in BANDS] + [99.0])
        rep.append("deployed band Fisher: " + ", ".join(f"{v:.1f}" for v in dbf))
        rep.append("Validate on a second recording before adopting.\n")
    rep.append("## Caveats\n")
    rep.append("- LOO windows come from the same session as their "
               "condition: accuracy ranks features, it does not prove "
               "cross-session generalization. Re-collect another day and "
               "re-run.")
    rep.append("- fs=200 reads a 400 Hz ODR part with no anti-alias "
               "filter: content in 100-200 Hz can fold into the spectrum "
               "(ADXL345 BW = ODR/2).")
    rep.append("- fmax is bin-quantized and was the documented settle-gate "
               "offender; check its within_std column.")
    rep.append("- The proxy mirrors max-component nearest-centroid "
               "matching; the TinyRL net adds one-step dynamics on top, "
               "not instead.")
    out = os.path.join(outdir, "report.md")
    with open(out, "w") as f:
        f.write("\n".join(rep) + "\n")
    return out


def write_plots(outdir, spectra, rows, names, fisher, bands_prop):
    try:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
    except ImportError:
        print("   (matplotlib unavailable -- plots skipped)")
        return
    fig, ax = plt.subplots(2, 1, figsize=(9, 7), sharex=True)
    for c in sorted(spectra):
        p, freqs = spectra[c]
        ax[0].semilogy(freqs, np.maximum(p.mean(axis=0), 1e-30), label=c)
    ax[0].set_ylabel("mean power (g^2, log)")
    ax[0].legend(fontsize=8)
    if bands_prop:
        _, prof, freqs = bands_prop
        ax[1].plot(freqs, prof)
        for e in bands_prop[0] or []:
            ax[1].axvline(e, color="r", ls="--", lw=0.8)
        ax[1].set_yscale("log")
        ax[1].set_ylabel("per-bin Fisher (log)")
    ax[1].set_xlabel("Hz")
    fig.tight_layout()
    fig.savefig(os.path.join(outdir, "spectra.png"), dpi=120)

    ok = [r for r in rows if r.get("acc") is not None
          and not np.isnan(r["acc"])]
    if ok:
        fig, ax = plt.subplots(figsize=(7, 4.5))
        ax.scatter([r["latency_s"] for r in ok], [r["acc"] for r in ok])
        for r in ok:
            ax.annotate(r["config"].replace("fs=", "").replace(" ", "/"),
                        (r["latency_s"], r["acc"]), fontsize=7,
                        xytext=(3, 3), textcoords="offset points")
        ax.set_xlabel("window latency (s)")
        ax.set_ylabel("LOO accuracy")
        fig.tight_layout()
        fig.savefig(os.path.join(outdir, "window_tradeoff.png"), dpi=120)

    fig, ax = plt.subplots(figsize=(7, 6))
    order = np.argsort(-fisher)[:15][::-1]
    ax.barh([names[i] for i in order], [max(fisher[i], 1e-3) for i in order])
    ax.set_xscale("log")
    ax.set_xlabel("Fisher (rel space, log)")
    fig.tight_layout()
    fig.savefig(os.path.join(outdir, "feature_scores.png"), dpi=120)
    print(f"   plots -> {outdir}/")


# =====================================================================
# 10b. export: raw captures -> model-testing feature CSV (no re-record)
# =====================================================================
def export_dataset(raw, args):
    """Regenerate the collect_data.py-equivalent feature dataset from raw
    captures: features computed by the same DSP the firmware runs (same
    DC blocker, window, band edges, non-overlapping windows), one row per
    2 s telemetry tick picking the LATEST COMPLETED window -- what the
    node's 1 Hz step would have put on [RAW]. t_s is time since the
    capture of that condition started (wall clock is not recoverable
    from the raw stream)."""
    sel = [f.strip() for f in args.features.split(",") if f.strip()]
    unknown = [f for f in sel if f not in BANK_ALL]
    if unknown:
        sys.exit(f"!! unknown features {unknown}; bank: {', '.join(BANK_ALL)}")
    print(f"== export: n={args.n} fs={args.fs:g} {args.win} | "
          f"features: {', '.join(sel)} | rows every {TELEMETRY_S:g} s")
    print("   (defaults = study recommendation; override with --n --fs "
          "--win --features)")
    names, feats, _ = compute_features(raw, args.n, args.fs, args.win)
    col = [BANK_ALL.index(f) for f in sel]
    ts = time.strftime("%Y%m%d_%H%M%S")
    outdir = args.outdir or os.path.join("data", "analysis")
    os.makedirs(outdir, exist_ok=True)
    path = os.path.join(outdir, f"export_{ts}.csv")
    total = 0
    with open(path, "w") as f:
        f.write("t_s,label," + ",".join(sel) + "\n")
        for cond in sorted(feats):
            M = feats[cond]
            k = len(M)
            dur = raw[cond].shape[1] / FS_IN
            nan_skips = 0
            rows = 0
            t = TELEMETRY_S
            while t <= dur and k > 0:
                # window i completes at (i+1) * n/fs s of raw time
                i = int(np.floor(t * args.fs / args.n + 1e-9)) - 1
                i = min(max(i, 0), k - 1)
                vec = M[i, col]
                if np.isnan(vec).any():
                    nan_skips += 1
                else:
                    f.write(f"{t:.1f},{cond},"
                            + ",".join(f"{v:.6g}" for v in vec) + "\n")
                    rows += 1
                t += TELEMETRY_S
            total += rows
            print(f"   {cond:22s} {rows:4d} rows ({dur:.1f} s, "
                  f"{k} windows)"
                  + (f"  [{nan_skips} NaN skips]" if nan_skips else ""))
    print(f"== {total} rows -> {path}")


# =====================================================================
# 11. main
# =====================================================================
def main():
    ap = argparse.ArgumentParser(
        description="vibration feature + FFT parameter selection study")
    ap.add_argument("input", nargs="?", default="data/raw",
                    help="directory of per-condition CSVs or one CSV")
    ap.add_argument("--capture", nargs="?", const="auto", default=None,
                    metavar="PORT", help="record raw samples from serial")
    ap.add_argument("--reset", action="store_true",
                    help="factory-wipe first: node IDLE, pure telemetry")
    ap.add_argument("--dur", type=float, default=30.0,
                    help="capture seconds per condition (default 30)")
    ap.add_argument("--campaign", default=None,
                    help="capture: comma list of conditions "
                         "(default: the full 7-condition campaign)")
    ap.add_argument("--check", metavar="CSV",
                    help="quality-check ONE raw capture (rate + spectrum "
                         "+ deployed features) instead of the study")
    ap.add_argument("--cal", default=None,
                    help="CAL condition for anchors (default op_stopped, "
                         "else first alphabetically)")
    ap.add_argument("--fs-in", type=float, default=FS_IN_DEF,
                    help="sample rate of the recordings (default 200)")
    ap.add_argument("--fft-ns", default=FFT_NS_DEF,
                    help="comma list of window lengths")
    ap.add_argument("--fs-list", default=FS_LIST_DEF,
                    help="comma list of analysis sample rates")
    ap.add_argument("--winfuncs", default=WINFUNCS_DEF,
                    help="comma list: hann,rect")
    ap.add_argument("--topk", type=int, default=4,
                    help="subset size to highlight (default 4)")
    ap.add_argument("--outdir", default=None,
                    help="output directory (default data/analysis/...)")
    ap.add_argument("--export", action="store_true",
                    help="export the model-testing feature CSV from the "
                         "captures instead of running the study")
    ap.add_argument("--n", type=int, default=64,
                    help="export: FFT window length (default 64, study rec)")
    ap.add_argument("--fs", type=float, default=100.0,
                    help="export: analysis rate (default 100, study rec)")
    ap.add_argument("--win", default="hann", choices=["hann", "rect"],
                    help="export: window function (default hann)")
    ap.add_argument("--features", default="rms,b2,rms_y,centroid",
                    help="export: comma list in model order "
                         "(default = the 2026-11-22 clean-capture re-lock)")
    ap.add_argument("--no-plot", action="store_true")
    args = ap.parse_args()

    global FS_IN, CAL
    FS_IN = args.fs_in

    if args.capture is not None:
        ts = time.strftime("%Y%m%d_%H%M%S")
        labels = [v.strip() for v in args.campaign.split(",")] \
            if args.campaign else None
        capture(args.capture, args.dur,
                os.path.join("data", "raw", f"cap_{ts}"), args.reset,
                labels)
        return

    if args.check:
        check_capture(args.check, n_fft=args.n, fs_eff=args.fs,
                      win=args.win, plot=not args.no_plot)
        return

    raw = load_raw(args.input)
    if len(raw) < 2:
        sys.exit("!! need >= 2 conditions to compare")
    CAL = args.cal if args.cal else (
        "op_stopped" if "op_stopped" in raw else sorted(raw)[0])
    if CAL not in raw:
        sys.exit(f"!! CAL condition '{CAL}' not in {sorted(raw)}")

    print(f"== inventory: {len(raw)} conditions, CAL={CAL}")
    for c, sig in raw.items():
        print(f"   {c:22s} {sig.shape[1]:7d} samples "
              f"({sig.shape[1] / FS_IN:6.1f} s, "
              f"{sig.shape[1] // BASELINE_N:4d} windows @ n=128)")
    thin = [c for c, s in raw.items() if s.shape[1] / FS_IN < 30]
    if thin:
        print(f"!!! under 30 s: {', '.join(thin)} -- repetitive data is "
              "fine, but these are thin for the 512-point row")

    if args.export:
        export_dataset(raw, args)
        return

    fft_ns = [int(v) for v in args.fft_ns.split(",")]
    fs_list = [float(v) for v in args.fs_list.split(",")]
    winfuncs = [v.strip() for v in args.winfuncs.split(",")]

    print("== window study ...")
    rows, keep = window_study(raw, fft_ns, fs_list, winfuncs, MIN_WIN)
    for r in rows:
        if r.get("acc") is None or np.isnan(r.get("acc", np.nan)):
            print(f"   {r['config']:26s}   --  ({r.get('note', '')})")
        else:
            print(f"   {r['config']:26s} acc={r['acc']:.4f} "
                  f"acc4={fmt(r.get('acc4'), 4)} "
                  f"lat={r['latency_s']:.2f}s "
                  f"within={r['mean_within']:.2f}/{r['max_within']:.2f}"
                  + (f"  dropped: {r['dropped']}"
                     if r.get("dropped", "-") != "-" else ""))

    valid = [r for r in rows if r.get("acc") is not None
             and not np.isnan(r["acc"])]
    if not valid:
        sys.exit("!! no config had enough windows; record longer")
    best = max(r["acc"] for r in valid)
    within = [r for r in valid if r["acc"] >= best - 0.02]
    rec = min(within, key=lambda r: (r["latency_s"], r["max_within"],
                                     -r["bin_hz"]))
    rec_label = rec["config"]
    base = [r for r in valid
            if r["config"] == cfg_label(BASELINE_N, FS_IN, "hann")]
    print(f"== recommendation: {rec_label}  "
          f"(best acc {best:.4f}; deployed baseline acc "
          f"{fmt(base[0]['acc'], 4) if base else '-'})")

    names, feats, spectra = keep[rec_label]
    # the ranking path must see the SAME usable columns the window study
    # scored: a feature with no bins at this (n, fs) is NaN everywhere
    # and its NaN anchors poison the whole rel space
    allM = np.vstack([v for v in feats.values() if len(v)])
    ok = ~np.isnan(allM).any(axis=0)
    if not ok.all():
        print("== dropped at this config (no bins): "
              + ", ".join(names[i] for i in np.where(~ok)[0]))
        names = [n for n, o in zip(names, ok) if o]
        feats = {c: v[:, ok] for c, v in feats.items()}
    rel, _, _ = build_rel(feats, CAL)
    fisher, C, W = fisher_scores(rel)
    wp = worst_pair(C, W)
    path = greedy_subset(rel, fisher, max(6, args.topk))[1]
    top_idx = [fi for fi, _, _ in path[:args.topk]]
    ev_top = loo_eval(rel, top_idx)
    ev_all = loo_eval(rel, list(range(len(names))))

    print("== feature ranking (Fisher in the anchored rel space):")
    for rank, i in enumerate(np.argsort(-fisher)[:10], 1):
        solo = loo_eval(rel, [int(i)])
        print(f"   {rank:2d}. {names[i]:10s} F={fisher[i]:9.2f} "
              f"solo={fmt(solo[0], 3) if solo else '-'}")
    print(f"== top {args.topk} subset: "
          + ", ".join(names[i] for i in top_idx)
          + f"  (subset acc {ev_top[0]:.4f}, "
            f"all-feature acc {ev_all[0]:.4f})")

    bands_prop = propose_bands(spectra)
    if bands_prop[0]:
        print("== band proposal (Hz): "
              + ", ".join(f"{e:.1f}" for e in bands_prop[0]))

    ts = time.strftime("%Y%m%d_%H%M%S")
    outdir = args.outdir or os.path.join(
        "data", "analysis", f"feature_selection_{ts}")
    rp = write_outputs(outdir, args, raw, rows, rec_label, names, rel,
                       fisher, wp, path, args.topk, bands_prop,
                       ev_top[1], sorted(rel), spectra)
    if not args.no_plot:
        write_plots(outdir, spectra, rows, names, fisher, bands_prop)
    print(f"== report: {rp}")


if __name__ == "__main__":
    main()
