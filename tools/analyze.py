#!/usr/bin/env python3
"""Analyze ESP32AntTest session logs and produce comparison plots.

Loads one or more host-merged session CSVs (as produced by mock_session.py or
by the Station/host serial bridge) and produces:

  - Per-step summary table (count, mean/std, p10/p50/p90, loss rate, source mix)
  - RSSI vs distance (range walk): MOB and STA, mean +/- std
  - RSSI vs angle (orientation): polar, MOB and STA, to expose asymmetry/nulls
  - Loss vs distance/angle
  - RSSI vs time: scatter, both directions, color-coded by source
  - Multi-session overlay: compare antenna profiles on the same protocol

The success criterion for the schema: a planted "good vs bad antenna"
difference must be clearly recoverable from these plots -- including the null
floor captured only in Mobile-buffered (source=MOB) samples during uplink
outage (the data beacon mode exists to recover).

Functions are importable (analyze_session(sessions, out_dir)) so the host
webserver can wrap the same logic in HTTP endpoints later.

Usage:
  python tools/analyze.py --sessions logs/*range_walk*.csv --out plots/
  python tools/analyze.py --sessions logs/*orientation*.csv --out plots/
"""
import argparse
import csv
import glob
import json
import math
import os
import statistics
from collections import defaultdict


def load_session(path):
    rows = []
    with open(path) as f:
        for r in csv.DictReader(f):
            for k in ("step_id", "seq", "timestamp_ms", "tx_mob", "tx_sta"):
                if r[k] != "":
                    r[k] = int(r[k])
                else:
                    r[k] = None
            for k in ("rssi_mob", "rssi_sta"):
                r[k] = float(r[k]) if r[k] not in ("", None) else None
            rows.append(r)
    # session_id + a label; pull profile name from filename if present
    sid = rows[0]["session_id"] if rows else os.path.basename(path)
    label = sid
    return {"path": path, "label": label, "rows": rows}


def step_summary(sess, beacon_hz, step_defs):
    """Per-step stats. step_defs: {step_id: {duration_s, ...}}."""
    by_step = defaultdict(list)
    expected = {}
    for r in sess["rows"]:
        by_step[r["step_id"]].append(r)
    for sid, sd in step_defs.items():
        expected[sid] = beacon_hz * sd["duration_s"]

    out = []
    for sid in sorted(step_defs):
        rs = by_step.get(sid, [])
        mob = [r["rssi_mob"] for r in rs if r["rssi_mob"] is not None]
        sta = [r["rssi_sta"] for r in rs if r["rssi_sta"] is not None]
        n_sta = sum(1 for r in rs if r["source"] == "STA")
        n_mob = sum(1 for r in rs if r["source"] == "MOB")
        exp = expected[sid]
        loss = 1.0 - (len(rs) / exp) if exp else float("nan")
        out.append({
            "step_id": sid,
            "n": len(rs),
            "n_sta": n_sta,
            "n_mob": n_mob,
            "expected": exp,
            "loss": loss,
            "rssi_mob_mean": statistics.mean(mob) if mob else None,
            "rssi_mob_std": statistics.pstdev(mob) if len(mob) > 1 else 0.0,
            "rssi_mob_min": min(mob) if mob else None,
            "rssi_mob_p10": percentile(mob, 10) if mob else None,
            "rssi_mob_p50": percentile(mob, 50) if mob else None,
            "rssi_mob_p90": percentile(mob, 90) if mob else None,
            "rssi_sta_mean": statistics.mean(sta) if sta else None,
            "rssi_sta_std": statistics.pstdev(sta) if len(sta) > 1 else 0.0,
            "rssi_sta_min": min(sta) if sta else None,
            "rssi_sta_p50": percentile(sta, 50) if sta else None,
        })
    return out


def percentile(xs, p):
    if not xs:
        return None
    s = sorted(xs)
    k = (len(s) - 1) * (p / 100.0)
    f = math.floor(k)
    c = math.ceil(k)
    if f == c:
        return s[int(k)]
    return s[f] + (s[c] - s[f]) * (k - f)


def step_meta(step_defs):
    """Return {step_id: {distance_m or angle_deg, type}} for plotting."""
    out = {}
    for sid, sd in step_defs.items():
        out[sid] = sd
    return out


def plot_range(sessions, step_defs, beacon_hz, out_dir):
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    fig, axes = plt.subplots(2, 1, figsize=(9, 8), sharex=True)
    for sess in sessions:
        summ = step_summary(sess, beacon_hz, step_defs)
        ds = [step_defs[s["step_id"]].get("distance_m") for s in summ]
        mob_m = [s["rssi_mob_mean"] for s in summ]
        sta_m = [s["rssi_sta_mean"] for s in summ]
        axes[0].plot(ds, mob_m, "o-", label=f"{sess['label']} MOB")
        axes[0].plot(ds, sta_m, "s--", label=f"{sess['label']} STA")
        loss = [s["loss"] * 100 for s in summ]
        axes[1].plot(ds, loss, "o-", label=sess["label"])
    axes[0].set_ylabel("RSSI (dBm)")
    axes[0].set_title("RSSI vs distance (range walk)")
    axes[0].legend(fontsize=8, ncol=2)
    axes[0].grid(True, alpha=0.3)
    axes[1].set_ylabel("Packet loss (%)")
    axes[1].set_xlabel("Distance (m)")
    axes[1].set_title("Loss vs distance")
    axes[1].set_ylim(-5, 105)
    axes[1].grid(True, alpha=0.3)
    axes[1].legend(fontsize=8)
    fig.tight_layout()
    p = os.path.join(out_dir, "range_walk.png")
    fig.savefig(p, dpi=110)
    plt.close(fig)
    return p


def plot_orientation(sessions, step_defs, beacon_hz, out_dir):
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    import numpy as np

    fig = plt.figure(figsize=(9, 9))
    ax = fig.add_subplot(111, polar=True)
    for sess in sessions:
        summ = step_summary(sess, beacon_hz, step_defs)
        angs = [math.radians(step_defs[s["step_id"]].get("angle_deg", 0)) for s in summ]
        # close the loop
        angs = angs + [angs[0]]
        mob_m = [s["rssi_mob_mean"] for s in summ] + [summ[0]["rssi_mob_mean"]]
        sta_m = [s["rssi_sta_mean"] for s in summ] + [summ[0]["rssi_sta_mean"]]
        ax.plot(angs, mob_m, "o-", label=f"{sess['label']} MOB (downlink)")
        ax.plot(angs, sta_m, "s--", label=f"{sess['label']} STA (uplink)")
    ax.set_theta_zero_location("N")
    ax.set_theta_direction(-1)
    ax.set_title("RSSI vs orientation (polar)\nMOB=downlink Station->Mobile, STA=uplink Mobile->Station",
                 fontsize=10)
    ax.legend(fontsize=8, loc="lower right", bbox_to_anchor=(1.25, 0.0))
    fig.tight_layout()
    p = os.path.join(out_dir, "orientation.png")
    fig.savefig(p, dpi=110)
    plt.close(fig)
    return p


def plot_timeseries(sessions, out_dir):
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    fig, ax = plt.subplots(figsize=(11, 5))
    for sess in sessions:
        ts = [r["timestamp_ms"] / 1000.0 for r in sess["rows"]]
        mob = [r["rssi_mob"] for r in sess["rows"]]
        sta = [r["rssi_sta"] for r in sess["rows"]]
        ax.scatter(ts, mob, s=6, label=f"{sess['label']} MOB", alpha=0.6)
        ax.scatter(ts, sta, s=6, label=f"{sess['label']} STA", alpha=0.6, marker="x")
    ax.set_xlabel("Time (s)")
    ax.set_ylabel("RSSI (dBm)")
    ax.set_title("RSSI vs time (full session)")
    ax.grid(True, alpha=0.3)
    ax.legend(fontsize=8, ncol=2)
    fig.tight_layout()
    p = os.path.join(out_dir, "timeseries.png")
    fig.savefig(p, dpi=110)
    plt.close(fig)
    return p


def print_summary(sessions, step_defs, beacon_hz):
    for sess in sessions:
        print(f"\n=== {sess['label']}  ({len(sess['rows'])} samples) ===")
        summ = step_summary(sess, beacon_hz, step_defs)
        hdr = f"{'step':>4} {'n':>4} {'STA':>4} {'MOB':>4} {'loss%':>6} | {'MOBmean':>7} {'MOBp50':>7} {'MOBmin':>7} | {'STAmean':>7} {'STAmin':>7}"
        print(hdr)
        print("-" * len(hdr))
        for s in summ:
            sd = step_defs[s["step_id"]]
            meta = sd.get("distance_m", sd.get("angle_deg", ""))
            mobm = f"{s['rssi_mob_mean']:.1f}" if s["rssi_mob_mean"] is not None else "  -  "
            mobp = f"{s['rssi_mob_p50']:.1f}" if s["rssi_mob_p50"] is not None else "  -  "
            mobmn = f"{s['rssi_mob_min']:.1f}" if s["rssi_mob_min"] is not None else "  -  "
            stam = f"{s['rssi_sta_mean']:.1f}" if s["rssi_sta_mean"] is not None else "  -  "
            stmn = f"{s['rssi_sta_min']:.1f}" if s["rssi_sta_min"] is not None else "  -  "
            print(f"{s['step_id']:>4} {s['n']:>4} {s['n_sta']:>4} {s['n_mob']:>4} {s['loss']*100:>5.0f}% | {mobm:>7} {mobp:>7} {mobmn:>7} | {stam:>7} {stmn:>7}   [{meta}]")


def main():
    ap = argparse.ArgumentParser(description="Analyze ESP32AntTest session logs.")
    ap.add_argument("--sessions", nargs="+", required=True, help="session CSV path(s) (glob ok)")
    ap.add_argument("--protocol", required=True, help="protocol JSON (for step defs + beacon rate)")
    ap.add_argument("--out", default="plots", help="output dir for PNGs")
    args = ap.parse_args()

    # expand globs
    paths = []
    for s in args.sessions:
        matches = glob.glob(s)
        paths.extend(matches if matches else [s])
    paths = sorted(dict.fromkeys(paths))

    protocol = json.load(open(args.protocol))
    step_defs = {s["step_id"]: s for s in protocol["steps"]}
    # beacon_hz is a profile param; assume 5 if not in protocol
    beacon_hz = protocol.get("beacon_hz", 5)

    sessions = [load_session(p) for p in paths]
    # Use a short label: filename stem
    for s in sessions:
        s["label"] = os.path.splitext(os.path.basename(s["path"]))[0]

    os.makedirs(args.out, exist_ok=True)
    print_summary(sessions, step_defs, beacon_hz)

    is_orient = any(s.get("type") == "orientation" for s in step_defs.values())
    if is_orient:
        p = plot_orientation(sessions, step_defs, beacon_hz, args.out)
    else:
        p = plot_range(sessions, step_defs, beacon_hz, args.out)
    print(f"\nplot: {p}")
    tp = plot_timeseries(sessions, args.out)
    print(f"plot: {tp}")


if __name__ == "__main__":
    main()
