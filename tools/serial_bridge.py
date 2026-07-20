"""Station serial bridge (SERIAL_PROTOCOL.md channels # / $ / >).

Thread-safe line reader + command sender. Used by tools/server.py.
"""
from __future__ import annotations

import json
import logging
import queue
import re
import threading
import time
from collections import deque
from dataclasses import dataclass, field
from datetime import datetime
from pathlib import Path
from typing import Any, Callable, Deque, Dict, List, Optional

LOG = logging.getLogger("serial_bridge")

_ANSI = re.compile(r"\x1b\[[0-9;]*m")


def strip_ansi(s: str) -> str:
    return _ANSI.sub("", s)


@dataclass
class BridgeState:
    connected: bool = False
    port: Optional[str] = None
    hello: Optional[dict] = None
    last_status: Optional[dict] = None
    last_error: Optional[str] = None
    session_id: Optional[str] = None
    session_active: bool = False
    protocol_id: Optional[str] = None
    row_count: int = 0
    last_row: Optional[str] = None
    events: Deque[dict] = field(default_factory=lambda: deque(maxlen=200))
    rows: Deque[str] = field(default_factory=lambda: deque(maxlen=500))


class StationBridge:
    def __init__(self, log_dir: Optional[Path] = None):
        self.state = BridgeState()
        self._ser = None
        self._thread: Optional[threading.Thread] = None
        self._stop = threading.Event()
        self._lock = threading.Lock()
        self._listeners: List[Callable[[str, Any], None]] = []
        self._evt_waiters: List[tuple[Optional[str], queue.Queue]] = []
        self._log_dir = Path(log_dir) if log_dir else None
        self._csv_fp = None
        self._csv_path: Optional[Path] = None

    def add_listener(self, fn: Callable[[str, Any], None]) -> None:
        self._listeners.append(fn)

    def _emit(self, kind: str, payload: Any) -> None:
        for fn in list(self._listeners):
            try:
                fn(kind, payload)
            except Exception:
                LOG.exception("listener error")

    def connect(self, port: str, baud: int = 115200) -> dict:
        import serial  # pyserial

        self.disconnect()
        ser = serial.Serial(port, baud, timeout=0.05)
        ser.reset_input_buffer()
        with self._lock:
            self._ser = ser
            self._stop.clear()
            self.state = BridgeState(connected=True, port=port)
        self._thread = threading.Thread(target=self._reader, name="sta-serial", daemon=True)
        self._thread.start()
        # small settle after open/USB noise
        time.sleep(0.3)
        try:
            hello = self.cmd({"cmd": "hello"}, wait_evt="hello", timeout=2.0)
            self.state.hello = hello
        except Exception as e:
            self.state.last_error = str(e)
            hello = None
        self._emit("connected", {"port": port, "hello": hello})
        return {"ok": True, "port": port, "hello": hello}

    def disconnect(self) -> None:
        self._stop.set()
        if self._thread and self._thread.is_alive():
            self._thread.join(timeout=2.0)
        self._thread = None
        self._close_csv()
        with self._lock:
            if self._ser is not None:
                try:
                    self._ser.close()
                except Exception:
                    pass
                self._ser = None
            self.state.connected = False
            self.state.port = None
        self._emit("disconnected", {})

    def _close_csv(self) -> None:
        if self._csv_fp:
            try:
                self._csv_fp.close()
            except Exception:
                pass
        self._csv_fp = None
        self._csv_path = None

    def _open_csv(self, session_id: str) -> None:
        self._close_csv()
        if not self._log_dir:
            return
        self._log_dir.mkdir(parents=True, exist_ok=True)
        path = self._log_dir / f"{session_id}.csv"
        self._csv_path = path
        self._csv_fp = open(path, "w", encoding="utf-8")
        # header matches SPEC §4 / logger.c
        self._csv_fp.write(
            "session_id,step_id,seq,timestamp_ms,datetime,mode,"
            "tx_mob,tx_sta,rssi_mob,rssi_sta,source,status\n"
        )
        self._csv_fp.flush()

    def _reader(self) -> None:
        buf = b""
        while not self._stop.is_set():
            with self._lock:
                ser = self._ser
            if ser is None:
                break
            try:
                n = ser.in_waiting
                chunk = ser.read(n or 1)
            except Exception as e:
                self.state.last_error = str(e)
                self.state.connected = False
                self._emit("error", {"reason": str(e)})
                break
            if not chunk:
                continue
            buf += chunk
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                try:
                    text = strip_ansi(line.decode("utf-8", "replace")).rstrip("\r")
                except Exception:
                    continue
                if text:
                    self._handle_line(text)

    def _handle_line(self, line: str) -> None:
        if line.startswith(">"):
            body = line[1:]
            self.state.rows.append(body)
            self.state.row_count += 1
            self.state.last_row = body
            if self._csv_fp:
                self._csv_fp.write(body + "\n")
                if self.state.row_count % 10 == 0:
                    self._csv_fp.flush()
            self._emit("row", body)
            return

        if line.startswith("$"):
            raw = line[1:].strip()
            try:
                obj = json.loads(raw)
            except json.JSONDecodeError:
                self._emit("diag", line)
                return
            if not isinstance(obj, dict):
                return
            self.state.events.append(obj)
            evt = obj.get("evt")
            if evt == "error":
                self.state.last_error = obj.get("reason", "error")
            if evt == "status":
                self.state.last_status = obj
                self.state.session_active = obj.get("state") == "session"
                if obj.get("session_id"):
                    self.state.session_id = obj.get("session_id")
            if evt == "session_started":
                self.state.session_active = True
                self.state.session_id = obj.get("session_id")
                self.state.row_count = 0
                if self.state.session_id:
                    self._open_csv(self.state.session_id)
            if evt in ("session_ended", "log_closed"):
                if evt == "session_ended":
                    self.state.session_active = False
                if self._csv_fp:
                    self._csv_fp.flush()
                if evt == "session_ended":
                    self._close_csv()
            if evt == "protocol_loaded":
                self.state.protocol_id = obj.get("protocol_id")
            self._emit("event", obj)
            # Wake matching cmd waiters (one-shot).
            with self._lock:
                keep: List[tuple[Optional[str], queue.Queue]] = []
                for want, q in self._evt_waiters:
                    if want is None or want == evt:
                        try:
                            q.put_nowait(obj)
                        except queue.Full:
                            pass
                    else:
                        keep.append((want, q))
                self._evt_waiters = keep
            return

        # banner / IDF logs
        if line.startswith("#") or line.startswith("I ") or line.startswith("W ") or line.startswith("E "):
            self._emit("diag", line)

    def wait_event(self, evt: Optional[str], timeout: float = 3.0) -> dict:
        q: queue.Queue = queue.Queue(maxsize=1)
        with self._lock:
            self._evt_waiters.append((evt, q))
        try:
            return q.get(timeout=timeout)
        except queue.Empty:
            with self._lock:
                self._evt_waiters = [w for w in self._evt_waiters if w[1] is not q]
            raise TimeoutError(f"timeout waiting for evt={evt!r}")

    def send_raw(self, obj: dict) -> None:
        line = "$ " + json.dumps(obj, separators=(",", ":")) + "\n"
        with self._lock:
            ser = self._ser
            if ser is None:
                raise RuntimeError("not connected")
            ser.write(line.encode("utf-8"))
            ser.flush()
        self._emit("tx", obj)

    def cmd(self, obj: dict, wait_evt: Optional[str] = None, timeout: float = 3.0) -> Optional[dict]:
        if wait_evt:
            q: queue.Queue = queue.Queue(maxsize=4)
            with self._lock:
                self._evt_waiters.append((wait_evt, q))
            self.send_raw(obj)
            deadline = time.time() + timeout
            last_err = None
            while time.time() < deadline:
                try:
                    msg = q.get(timeout=max(0.05, deadline - time.time()))
                except queue.Empty:
                    break
                if msg.get("evt") == "error":
                    last_err = msg
                    continue
                if wait_evt is None or msg.get("evt") == wait_evt:
                    with self._lock:
                        self._evt_waiters = [w for w in self._evt_waiters if w[1] is not q]
                    return msg
            with self._lock:
                self._evt_waiters = [w for w in self._evt_waiters if w[1] is not q]
            if last_err:
                raise RuntimeError(last_err.get("reason", "error"))
            raise TimeoutError(f"timeout waiting for evt={wait_evt!r}")
        self.send_raw(obj)
        return None

    def settime_now(self) -> dict:
        iso = datetime.now().strftime("%Y-%m-%dT%H:%M:%S")
        return self.cmd({"cmd": "settime", "iso": iso}, wait_evt="time_ok", timeout=2.0) or {}

    def load_protocol(self, protocol: dict) -> dict:
        return self.cmd(
            {"cmd": "load_protocol", "protocol": protocol},
            wait_evt="protocol_loaded",
            timeout=3.0,
        ) or {}

    def start_session(self, mode: str = "WIFI", tx_mob: int = 17, tx_sta: int = 17) -> dict:
        return self.cmd(
            {"cmd": "start_session", "mode": mode, "tx_mob": tx_mob, "tx_sta": tx_sta},
            wait_evt="session_started",
            timeout=5.0,
        ) or {}

    def end_session(self) -> dict:
        return self.cmd({"cmd": "end_session"}, wait_evt="session_ended", timeout=5.0) or {}

    def status(self) -> dict:
        return self.cmd({"cmd": "status"}, wait_evt="status", timeout=2.0) or {}

    def snapshot(self) -> dict:
        s = self.state
        return {
            "connected": s.connected,
            "port": s.port,
            "hello": s.hello,
            "last_status": s.last_status,
            "last_error": s.last_error,
            "session_id": s.session_id,
            "session_active": s.session_active,
            "protocol_id": s.protocol_id,
            "row_count": s.row_count,
            "last_row": s.last_row,
            "csv_path": str(self._csv_path) if self._csv_path else None,
            "recent_events": list(s.events)[-20:],
            "recent_rows": list(s.rows)[-30:],
        }
