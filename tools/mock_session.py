#!/usr/bin/env python3
"""Synthetic session log generator for ESP32AntTest (beacon-mode model).

Models the v1 firmware sampling model agreed for the project: both boards
transmit beacons at a fixed rate; each board logs every beacon it can decode
(RSSI above its decode threshold). Each beacon piggybacks `rssi_local` (the
RSSI the sender measured from the other board's last beacon), so a single
received beacon yields BOTH directional RSSI values (see ADR-001).

This script emits the **host-merged** canonical log, i.e. the log the host
webserver would assemble from Station's serial stream plus Mobile's
forwarded backlog. Each row is tagged with provenance via the `source` column:

  source=STA  Station received Mobile's beacon. rssi_mob (piggybacked by
              Mobile) and rssi_sta (measured by Station) both present.
              This is the in-range common case; Station's log is complete
              from beacons alone.
  source=MOB  Mobile received Station's beacon but Station did NOT receive
              Mobile's (uplink outage / Mobile in a TX null). Mobile buffered
              rssi_mob; rssi_sta is empty (Station heard nothing to piggyback).
              This is the null-floor data beacon mode is meant to recover;
              it would be a bare TIMEOUT (no RSSI at all) under request-response.
  (no row)    Neither board received the other -> full outage. Loss is inferred
              by analyze.py from sample count vs expected (beacon_hz * duration).

The merge is joined by step_id, not clock sync -- both boards know the
protocol, so the step index is the join key.

Usage:
  python tools/mock_session.py --protocol protocols/range_walk.json \
      --profile profiles/good_antenna.json --out logs/ [--seed 42]

The antenna `profile` models the Mobile board's antenna (the thing under
test) plus shared RF parameters. Generate two sessions with different
profiles and run analyze.py to confirm the planted differences survive into
the plots.
"""
import argparse
import csv
import json
import math
import os
import random
from datetime import datetime


def load_json(path):
    with open(path) as f:
        return json.load(f)


def path_loss_db(d_m, n):
    """Log-distance path loss at 2.4 GHz, d in meters, exponent n (2=free space).

    PL(d) = PL0(1m) + 10*n*log10(d). PL0 at 1m @ 2.4 GHz ~= 40.05 dB.
    """
    d = max(d_m, 1.0)
    return 40.05 + 10.0 * n * math.log10(d)


def pattern_gain_db(angle_deg, depth_db, null_angle_deg):
    """Antenna pattern relative gain (dB) vs rotation about one axis.

    Lobe (0 dB, maximum) at null_angle + 180; deepest null (-depth_db) at
    null_angle. Smooth cosine model: -(depth/2)*(1 + cos(angle - null)).
    """
    a = math.radians(angle_deg - null_angle_deg)
    return -(depth_db / 2.0) * (1.0 + math.cos(a))


def rssi_uplink(tx_mob, gain_mob, gain_sta, pl):
    """RSSI at Station of Mobile's beacon (RSSI_STA, uplink Mobile->Station)."""
    return tx_mob + gain_mob + gain_sta - pl


def rssi_downlink(tx_sta, gain_sta, gain_mob, pl):
    """RSSI at Mobile of Station's beacon (RSSI_MOB, downlink Station->Mobile)."""
    return tx_sta + gain_sta + gain_mob - pl


def simulate(protocol, profile, session_id, start_dt, rng):
    """Yield merged-log rows as dicts."""
    beacon_hz = profile["beacon_hz"]
    interval_ms = int(round(1000.0 / beacon_hz))
    jitter = profile["jitter_db"]
    fade_db = profile["fading_db"]
    fade_hz = profile["fading_hz"]
    thr_sta = profile["decode_threshold_sta_dbm"]
    thr_mob = profile["decode_threshold_mob_dbm"]
    mob_gain = profile["mob_gain_dbi"]
    sta_gain = profile["sta_gain_dbi"]
    depth = profile["mob_pattern_depth_db"]
    null_ang = profile["mob_null_angle_deg"]
    n_exp = profile["path_loss_exp"]

    tx_mob = protocol.get("tx_mob", 17)
    tx_sta = protocol.get("tx_sta", 20)
    fixed_d = protocol.get("fixed_distance_m")

    t_ms = 0
    seq = 0
    last_mob_meas_of_sta = None  # freshest RSSI_MOB Mobile has (for piggyback)

    for step in protocol["steps"]:
        sid = step["step_id"]
        dur_ms = int(step["duration_s"] * 1000)
        if step["type"] == "distance":
            d = step["distance_m"]
            ang = 0  # range walk: Mobile aimed at lobe (assume optimal aim)
        elif step["type"] == "orientation":
            d = fixed_d if fixed_d is not None else 5.0
            ang = step["angle_deg"]
        else:
            raise ValueError(f"unknown step type: {step['type']}")

        pl = path_loss_db(d, n_exp)
        gpatt = pattern_gain_db(ang, depth, null_ang)
        g_mob = mob_gain + gpatt  # reciprocity: same pattern on TX and RX

        step_end = t_ms + dur_ms
        # simulate each beacon interval within the step
        while t_ms < step_end:
            # slow multipath fading (position/time dependent), common-mode-ish
            fade = fade_db * math.sin(2 * math.pi * fade_hz * (t_ms / 1000.0)
                                       + rng.random() * 0.0)
            j_mob = rng.uniform(-jitter / 2, jitter / 2)
            j_sta = rng.uniform(-jitter / 2, jitter / 2)

            r_mob = rssi_downlink(tx_sta, sta_gain, g_mob, pl) + fade + j_mob
            r_sta = rssi_uplink(tx_mob, g_mob, sta_gain, pl) + fade + j_sta

            sta_heard = r_sta >= thr_sta
            mob_heard = r_mob >= thr_mob

            ts = start_dt + timedelta_ms(t_ms)

            if sta_heard:
                # Station received Mobile's beacon: complete row.
                # Piggyback rssi_mob = Mobile's freshest measurement of Station.
                pigg = round(last_mob_meas_of_sta, 1) if last_mob_meas_of_sta is not None else ""
                yield {
                    "session_id": session_id,
                    "step_id": sid,
                    "seq": seq,
                    "timestamp_ms": t_ms,
                    "datetime": ts,
                    "mode": protocol["mode"],
                    "tx_mob": tx_mob,
                    "tx_sta": tx_sta,
                    "rssi_mob": pigg,
                    "rssi_sta": round(r_sta, 1),
                    "source": "STA",
                    "status": "OK",
                }
                seq += 1
            elif mob_heard:
                # Uplink outage, downlink still decodable: Mobile buffers.
                yield {
                    "session_id": session_id,
                    "step_id": sid,
                    "seq": seq,
                    "timestamp_ms": t_ms,
                    "datetime": ts,
                    "mode": protocol["mode"],
                    "tx_mob": tx_mob,
                    "tx_sta": tx_sta,
                    "rssi_mob": round(r_mob, 1),
                    "rssi_sta": "",
                    "source": "MOB",
                    "status": "OK",
                }
                seq += 1
            # else: full outage -> no row (loss inferred from count)

            # Mobile always updates its freshest measurement when it hears Station
            if mob_heard:
                last_mob_meas_of_sta = r_mob

            t_ms += interval_ms

    return


def timedelta_ms(ms):
    from datetime import timedelta
    return timedelta(milliseconds=ms)


def main():
    ap = argparse.ArgumentParser(description="Generate a synthetic ESP32AntTest session log.")
    ap.add_argument("--protocol", required=True, help="path to protocol JSON")
    ap.add_argument("--profile", required=True, help="path to antenna profile JSON")
    ap.add_argument("--out", default="logs", help="output directory")
    ap.add_argument("--seed", type=int, default=None, help="RNG seed for reproducibility")
    args = ap.parse_args()

    protocol = load_json(args.protocol)
    profile = load_json(args.profile)
    rng = random.Random(args.seed)

    start_dt = datetime.now().replace(microsecond=0)
    session_id = start_dt.strftime("%Y%m%d_%H%M%S")

    rows = list(simulate(protocol, profile, session_id, start_dt, rng))

    os.makedirs(args.out, exist_ok=True)
    # File name includes profile + protocol so contrasting runs don't clobber.
    fname = f"{session_id}_{profile['name'].split()[0].lower()}_{protocol['protocol_id']}.csv"
    out_path = os.path.join(args.out, fname)

    cols = ["session_id", "step_id", "seq", "timestamp_ms", "datetime",
            "mode", "tx_mob", "tx_sta", "rssi_mob", "rssi_sta", "source", "status"]
    with open(out_path, "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=cols)
        w.writeheader()
        w.writerows(rows)

    n_sta = sum(1 for r in rows if r["source"] == "STA")
    n_mob = sum(1 for r in rows if r["source"] == "MOB")
    print(f"wrote {out_path}")
    print(f"  {len(rows)} samples  (STA: {n_sta}, MOB-buffered: {n_mob})")
    print(f"  session_id={session_id}")


if __name__ == "__main__":
    main()
