# tools/ — Host-side tooling

Host instrument UI, serial bridge, analysis CLI, and the synthetic-data
mockup used to lock the log schema (ADR-004).

## What's here

- **`server.py`** — FastAPI host webserver: Station serial, protocol
  **list/load** from `protocols/` (no in-browser editor — edit JSON in-repo),
  start/end session, live `>` / `$` tail (SSE), vanilla JS under `static/`.
  CSV mirror: `logs/host/<session_id>.csv`. Plot UI still TODO.
- **`serial_bridge.py`** — thread-safe Station serial client
  (`docs/SERIAL_PROTOCOL.md`). Exclusive port open + hello retries.
- **`analyze.py`** — per-step stats + plots (range / orientation polar /
  time / loss). Importable; wrap in HTTP later. Works on mock or host CSVs.
- **`mock_session.py`** — synthetic beacon-mode session CSV from a protocol
  + antenna profile (design artifact that validated the schema).
- **`requirements.txt`** — fastapi, uvicorn, pyserial, matplotlib.
- **`static/`** — connect / session / live-tail UI (no build step).

## Host webserver (day-to-day RF runs)

**Preferred:** run on the machine that has **Station USB** (often the
hypervisor/host PC once firmware is already flashed). Shared project tree is
enough — **no ESP-IDF / build VM** required to operate.

```bash
# detach Station from libvirt passthrough if the guest was owning USB
cd /path/to/ESP32AntTest
python3 -m venv .venv && . .venv/bin/activate
pip install -r tools/requirements.txt
python tools/server.py --port 8000   # binds 0.0.0.0 by default
# browser: http://127.0.0.1:8000/
# Connect → Station port (ttyUSB0 Config A) → protocol → Start session
# Mobile: Quick-Check, join SoftAP, short-press guided steps
```

Only **one** process may own Station's serial port. If `hello` times out,
stop leftover `server.py` / `idf monitor` / `screen`.

**Alternate — server stays in headless build VM** (Station USB passed
through): open the guest LAN IP from the host (`hostname -I` in the guest,
e.g. `http://192.168.122.80:8000/`), or:

```bash
ssh -L 8000:127.0.0.1:8000 user@192.168.122.80
# browser: http://127.0.0.1:8000/
```

See [docs/DEVENV.md](../docs/DEVENV.md).

After a session, plot offline:

```bash
python tools/analyze.py --sessions 'logs/host/<session_id>.csv' \
  --protocol protocols/range_walk.json --out plots/
```

API sketch: `GET /api/ports`, `POST /api/connect`, `GET /api/protocols`,
`POST /api/start_session`, `POST /api/end_session`, `GET /api/stream` (SSE),
`GET /api/state`.

**Deferred:** in-browser protocol authoring. Tweaks go in `protocols/*.json`.

## Mockup / analyze CLI (design artifact + offline plots)

```bash
# from repo root, with the venv active
python tools/mock_session.py --protocol protocols/range_walk.json \
    --profile profiles/good_antenna.json --out logs/ --seed 1
python tools/analyze.py --sessions "logs/*range_walk*.csv" \
    --protocol protocols/range_walk.json --out plots/
```

The mockup's RF model is intentionally tunable in profile JSONs — a model,
not a measurement. Generated root `logs/` / `plots/` are design-process
evidence.
