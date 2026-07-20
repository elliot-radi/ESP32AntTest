#!/usr/bin/env python3
"""ESP32AntTest host webserver — first slice.

FastAPI + Station serial bridge + vanilla JS UI:
  - connect / hello / settime
  - list & load protocols from protocols/
  - start_session / end_session
  - live tail of > rows and $ events (SSE)

Usage (repo root):
  python -m venv .venv && . .venv/bin/activate
  pip install -r tools/requirements.txt
  python tools/server.py --port 8000
  # open http://127.0.0.1:8000/
"""
from __future__ import annotations

import argparse
import asyncio
import json
import logging
import sys
from pathlib import Path
from typing import Any, Optional

from contextlib import asynccontextmanager

from fastapi import FastAPI, HTTPException
from fastapi.responses import FileResponse, StreamingResponse
from fastapi.staticfiles import StaticFiles
from pydantic import BaseModel, Field

ROOT = Path(__file__).resolve().parents[1]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT / "tools"))

from serial_bridge import StationBridge  # noqa: E402

LOG = logging.getLogger("server")
PROTO_DIR = ROOT / "protocols"
LOG_DIR = ROOT / "logs" / "host"
STATIC_DIR = Path(__file__).resolve().parent / "static"

bridge = StationBridge(log_dir=LOG_DIR)
# asyncio queue of SSE payloads (str)
_sse_queues: list[asyncio.Queue] = []
_main_loop: Optional[asyncio.AbstractEventLoop] = None


def _broadcast(kind: str, payload: Any) -> None:
    msg = json.dumps({"kind": kind, "data": payload}, separators=(",", ":"))
    loop = _main_loop
    if loop is None:
        return
    for q in list(_sse_queues):
        try:
            loop.call_soon_threadsafe(q.put_nowait, msg)
        except Exception:
            pass


def _on_bridge(kind: str, payload: Any) -> None:
    _broadcast(kind, payload)


bridge.add_listener(_on_bridge)


@asynccontextmanager
async def lifespan(app: FastAPI):
    global _main_loop
    _main_loop = asyncio.get_running_loop()
    LOG_DIR.mkdir(parents=True, exist_ok=True)
    yield
    bridge.disconnect()


app = FastAPI(title="ESP32AntTest Host", version="0.1.0", lifespan=lifespan)


class ConnectBody(BaseModel):
    port: str = Field(..., examples=["/dev/ttyUSB0"])
    baud: int = 115200


class ProtocolBody(BaseModel):
    protocol: dict


class StartBody(BaseModel):
    mode: str = "WIFI"
    tx_mob: int = 17
    tx_sta: int = 17
    settime: bool = True
    protocol: Optional[dict] = None
    protocol_file: Optional[str] = None  # basename in protocols/


@app.get("/api/health")
def health():
    return {"ok": True, "root": str(ROOT)}


@app.get("/api/state")
def state():
    return bridge.snapshot()


@app.get("/api/ports")
def ports():
    """Best-effort list of serial ports."""
    try:
        from serial.tools import list_ports
        return {
            "ports": [
                {"device": p.device, "description": p.description or "", "hwid": p.hwid or ""}
                for p in list_ports.comports()
            ]
        }
    except Exception as e:
        return {"ports": [], "error": str(e)}


@app.post("/api/connect")
def connect(body: ConnectBody):
    try:
        return bridge.connect(body.port, body.baud)
    except Exception as e:
        raise HTTPException(400, str(e)) from e


@app.post("/api/disconnect")
def disconnect():
    bridge.disconnect()
    return {"ok": True}


@app.post("/api/hello")
def hello():
    try:
        return bridge.cmd({"cmd": "hello"}, wait_evt="hello", timeout=2.0)
    except Exception as e:
        raise HTTPException(400, str(e)) from e


@app.post("/api/settime")
def settime():
    try:
        return bridge.settime_now()
    except Exception as e:
        raise HTTPException(400, str(e)) from e


@app.get("/api/protocols")
def list_protocols():
    out = []
    if PROTO_DIR.is_dir():
        for p in sorted(PROTO_DIR.glob("*.json")):
            try:
                data = json.loads(p.read_text())
            except Exception:
                data = {}
            out.append(
                {
                    "file": p.name,
                    "protocol_id": data.get("protocol_id") or data.get("id") or p.stem,
                    "steps": len(data.get("steps") or []),
                    "path": str(p.relative_to(ROOT)),
                }
            )
    return {"protocols": out}


@app.get("/api/protocols/{name}")
def get_protocol(name: str):
    path = PROTO_DIR / name
    if not path.is_file():
        # allow bare id without .json
        path = PROTO_DIR / f"{name}.json"
    if not path.is_file():
        raise HTTPException(404, f"protocol not found: {name}")
    return json.loads(path.read_text())


@app.post("/api/load_protocol")
def load_protocol(body: ProtocolBody):
    try:
        return bridge.load_protocol(body.protocol)
    except Exception as e:
        raise HTTPException(400, str(e)) from e


@app.post("/api/start_session")
def start_session(body: StartBody):
    try:
        if body.settime:
            bridge.settime_now()
        proto = body.protocol
        if proto is None and body.protocol_file:
            path = PROTO_DIR / body.protocol_file
            if not path.is_file():
                path = PROTO_DIR / f"{body.protocol_file}.json"
            if not path.is_file():
                raise HTTPException(404, f"protocol file not found: {body.protocol_file}")
            proto = json.loads(path.read_text())
        if proto is not None:
            bridge.load_protocol(proto)
        started = bridge.start_session(body.mode, body.tx_mob, body.tx_sta)
        return {"started": started, "state": bridge.snapshot()}
    except HTTPException:
        raise
    except Exception as e:
        raise HTTPException(400, str(e)) from e


@app.post("/api/end_session")
def end_session():
    try:
        ended = bridge.end_session()
        return {"ended": ended, "state": bridge.snapshot()}
    except Exception as e:
        raise HTTPException(400, str(e)) from e


@app.post("/api/status")
def status():
    try:
        return bridge.status()
    except Exception as e:
        raise HTTPException(400, str(e)) from e


@app.get("/api/stream")
async def stream():
    """Server-Sent Events: live bridge traffic."""
    q: asyncio.Queue = asyncio.Queue(maxsize=200)
    _sse_queues.append(q)

    async def gen():
        try:
            # initial snapshot
            yield f"data: {json.dumps({'kind': 'state', 'data': bridge.snapshot()})}\n\n"
            while True:
                try:
                    msg = await asyncio.wait_for(q.get(), timeout=20.0)
                    yield f"data: {msg}\n\n"
                except asyncio.TimeoutError:
                    yield f"data: {json.dumps({'kind': 'ping', 'data': {}})}\n\n"
        finally:
            if q in _sse_queues:
                _sse_queues.remove(q)

    return StreamingResponse(gen(), media_type="text/event-stream")


# Static UI
if STATIC_DIR.is_dir():
    app.mount("/static", StaticFiles(directory=str(STATIC_DIR)), name="static")


@app.get("/")
def index():
    index_path = STATIC_DIR / "index.html"
    if not index_path.is_file():
        raise HTTPException(404, "tools/static/index.html missing")
    return FileResponse(index_path)


def main():
    logging.basicConfig(level=logging.INFO, format="%(levelname)s %(name)s: %(message)s")
    ap = argparse.ArgumentParser(description="ESP32AntTest host webserver")
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=8000)
    ap.add_argument("--reload", action="store_true")
    args = ap.parse_args()
    import uvicorn

    uvicorn.run(
        "server:app" if args.reload else app,
        host=args.host,
        port=args.port,
        reload=args.reload,
        factory=False,
        app_dir=str(Path(__file__).resolve().parent) if args.reload else None,
    )


if __name__ == "__main__":
    main()
