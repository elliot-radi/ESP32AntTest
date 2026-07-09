# tools/ — Host-side tooling and the design mockup

This directory holds the host-side tooling for ESP32AntTest. It currently
contains a **synthetic-data mockup** used to validate the log schema and
analysis pipeline *before any firmware exists* — a design-process artifact,
kept committed so future readers can see how the schema was derived.

## What's here

- **`mock_session.py`** — generates a synthetic session CSV from a protocol
  JSON + an antenna profile JSON. Models the v1 **beacon-mode** sampling
  agreed for the project: both boards beacon at a fixed rate; each board logs
  every beacon it decodes; each beacon piggybacks `rssi_local` so one received
  beacon yields both directional RSSI values. Emits the host-merged canonical
  log, including `source=MOB` rows captured by Mobile during uplink outages
  (the null-floor data beacon mode exists to recover). See ADR-004.
- **`analyze.py`** — loads one or more session CSVs and produces per-step
  summary stats + plots: RSSI vs distance (range walk), RSSI vs angle (polar,
  both directions — exposes radiation-pattern asymmetry), loss vs distance/
  angle, and RSSI vs time. Functions are importable so the production
  webserver (`server.py`, to be built) can wrap the same logic in HTTP endpoints.
- **`requirements.txt`** — `matplotlib` (numpy comes along as a dep). Use a
  project venv: `python3 -m venv .venv && . .venv/bin/activate && pip install -r tools/requirements.txt`.
- **`server.py`** *(planned, not yet written)* — the production FastAPI
  webserver: protocol authoring UI, session execution (serial bridge to
  Station), live view, results/plots. Wraps `analyze.py`.

## The mockup as a design artifact

The mockup is not throwaway scratch — it is the prototype of the webserver's
backend and the statement of what we expect to measure. The synthetic antenna
profiles plant a "good vs bad antenna" difference (different gains, radiation
pattern depths, null angles); the analysis recovers those planted differences
from the generated logs, including the null floor captured only in
`source=MOB` rows. That recovery is the success criterion that locked the
schema. The generated `logs/` and `plots/` in the repo root are example
outputs from this mockup and are committed as design-process evidence.

## Usage

```bash
# from repo root, with the venv active
python tools/mock_session.py --protocol protocols/range_walk.json \
    --profile profiles/good_antenna.json --out logs/ --seed 1
python tools/mock_session.py --protocol protocols/range_walk.json \
    --profile profiles/bad_antenna.json  --out logs/ --seed 2
python tools/analyze.py --sessions "logs/*range_walk*.csv" \
    --protocol protocols/range_walk.json --out plots/

# orientation sweep
python tools/mock_session.py --protocol protocols/orientation_z.json \
    --profile profiles/good_antenna.json --out logs/ --seed 3
python tools/mock_session.py --protocol protocols/orientation_z.json \
    --profile profiles/bad_antenna.json  --out logs/ --seed 4
python tools/analyze.py --sessions "logs/*orientation*.csv" \
    --protocol protocols/orientation_z.json --out plots/
```

The mockup's RF model (path loss, antenna pattern, jitter, fading, decode
thresholds) is intentionally tunable in the profile JSONs — the default
tuning compresses outages into short distances so the asymmetric-capture
behavior is visible without a field test. It is a model, not a measurement.
