/* vanilla JS — ESP32AntTest host slice 1 */
const $ = (id) => document.getElementById(id);

function appendLive(el, text, cls) {
  const line = document.createElement("div");
  if (cls) line.className = cls;
  line.textContent = text;
  el.appendChild(line);
  while (el.childNodes.length > 400) el.removeChild(el.firstChild);
  el.scrollTop = el.scrollHeight;
}

async function api(path, opts = {}) {
  const r = await fetch(path, {
    headers: { "Content-Type": "application/json", ...(opts.headers || {}) },
    ...opts,
  });
  const text = await r.text();
  let body;
  try {
    body = text ? JSON.parse(text) : {};
  } catch {
    body = { raw: text };
  }
  if (!r.ok) {
    const msg = body.detail || body.reason || text || r.statusText;
    throw new Error(typeof msg === "string" ? msg : JSON.stringify(msg));
  }
  return body;
}

function defaultStationPort(ports) {
  /* Config A Station = CP210x /dev/ttyUSB*. Prefer that over ttyACM (Mobile). */
  const usb = ports.find((p) => (p.device || "").startsWith("/dev/ttyUSB"));
  if (usb) return usb.device;
  const cp = ports.find((p) => p.vid === 0x10c4 || p.vid === 4292);
  if (cp) return cp.device;
  return ports[0] && ports[0].device;
}

async function refreshPorts() {
  const showAll = $("showAllPorts") && $("showAllPorts").checked;
  const data = await api("/api/ports" + (showAll ? "?all=1" : ""));
  const sel = $("portSelect");
  const prev = sel.value;
  sel.innerHTML = "";
  const ports = data.ports || [];
  if (!ports.length) {
    const o = document.createElement("option");
    o.value = "";
    o.textContent = "(none found — plug Station USB / passthrough?)";
    sel.appendChild(o);
  } else {
    for (const p of ports) {
      const o = document.createElement("option");
      o.value = p.device;
      const desc = p.description || p.manufacturer || "";
      const tag =
        (p.device || "").startsWith("/dev/ttyUSB") ? " [Station?]" :
        (p.device || "").startsWith("/dev/ttyACM") ? " [Mobile?]" : "";
      o.textContent = `${p.device} — ${desc}${tag}`;
      sel.appendChild(o);
    }
  }
  const preferred = defaultStationPort(ports);
  if (prev && [...sel.options].some((o) => o.value === prev)) {
    sel.value = prev;
  } else if (preferred) {
    sel.value = preferred;
  }
  /* Do not auto-fill override — empty means "use dropdown". */
}

function selectedPort() {
  const override = ($("portCustom").value || "").trim();
  if (override) return override;
  return ($("portSelect").value || "").trim();
}

async function refreshProtocols() {
  const data = await api("/api/protocols");
  const sel = $("protoSelect");
  sel.innerHTML = "";
  for (const p of data.protocols || []) {
    const o = document.createElement("option");
    o.value = p.file;
    o.textContent = `${p.protocol_id} (${p.steps} steps) — ${p.file}`;
    sel.appendChild(o);
  }
}

function paintState(s) {
  if (!s) return;
  const bits = [
    `connected=${s.connected}`,
    s.port ? `port=${s.port}` : "",
    s.hello
      ? `fw=${s.hello.fw || "?"} mac=${s.hello.mac || "?"}`
      : s.connected
        ? "hello=pending/failed"
        : "",
    s.session_active ? `SESSION ${s.session_id || ""}` : "quickcheck",
    s.protocol_id ? `protocol=${s.protocol_id}` : "",
    s.csv_path ? `csv=${s.csv_path}` : "",
  ].filter(Boolean);
  let text = bits.join(" | ");
  if (s.last_error) text += `\nerr: ${s.last_error}`;
  $("connInfo").textContent = text;
  $("rowCount").textContent = String(s.row_count || 0);
  if (s.last_status) {
    $("sessInfo").textContent = JSON.stringify(s.last_status);
  }
}

async function pullState() {
  try {
    paintState(await api("/api/state"));
  } catch (e) {
    $("connInfo").textContent = String(e.message || e);
  }
}

async function connect() {
  const port = selectedPort();
  if (!port) {
    alert("Choose a port from the list (or type an override path)");
    return;
  }
  $("connInfo").textContent = `Connecting to ${port}…`;
  try {
    const res = await api("/api/connect", {
      method: "POST",
      body: JSON.stringify({ port }),
    });
    if (res.hello) {
      $("connInfo").textContent =
        `connected ${port}  fw=${res.hello.fw || "?"} mac=${res.hello.mac || "?"}`;
    } else {
      $("connInfo").textContent =
        `port open on ${port} but hello failed: ${res.error || "unknown"}\n` +
        `Stop any other process using the port (old server.py / idf monitor), then Connect again.`;
    }
    await pullState();
  } catch (e) {
    $("connInfo").textContent = `Connect failed (${port}): ${e.message}`;
    alert(`Connect failed (${port}): ` + e.message);
  }
}

async function disconnect() {
  await api("/api/disconnect", { method: "POST", body: "{}" });
  await pullState();
}

async function startSession() {
  try {
    const body = {
      mode: $("modeSelect").value,
      tx_mob: Number($("txMob").value),
      tx_sta: Number($("txSta").value),
      settime: true,
      protocol_file: $("protoSelect").value,
    };
    const res = await api("/api/start_session", {
      method: "POST",
      body: JSON.stringify(body),
    });
    $("sessInfo").textContent = JSON.stringify(res.started || res, null, 2);
    paintState(res.state);
  } catch (e) {
    alert("Start failed: " + e.message);
  }
}

async function endSession() {
  try {
    const res = await api("/api/end_session", { method: "POST", body: "{}" });
    $("sessInfo").textContent = JSON.stringify(res.ended || res, null, 2);
    paintState(res.state);
  } catch (e) {
    alert("End failed: " + e.message);
  }
}

function openStream() {
  const es = new EventSource("/api/stream");
  es.onmessage = (ev) => {
    let msg;
    try {
      msg = JSON.parse(ev.data);
    } catch {
      return;
    }
    const { kind, data } = msg;
    if (kind === "state") {
      paintState(data);
      return;
    }
    if (kind === "row") {
      appendLive($("live"), data, "line-row");
      const n = Number($("rowCount").textContent || "0") + 1;
      $("rowCount").textContent = String(n);
      return;
    }
    if (kind === "event") {
      const cls = data && data.evt === "error" ? "line-err" : "line-evt";
      appendLive($("events"), JSON.stringify(data), cls);
      if (data && (data.evt === "status" || data.evt === "session_started" || data.evt === "session_ended")) {
        pullState();
      }
      return;
    }
    if (kind === "tx") {
      appendLive($("events"), ">> " + JSON.stringify(data), "line-evt");
      return;
    }
    if (kind === "error") {
      appendLive($("events"), "ERR " + JSON.stringify(data), "line-err");
    }
  };
  es.onerror = () => {
    /* browser will retry EventSource */
  };
}

function bind() {
  $("btnRefreshPorts").onclick = () => refreshPorts().catch(console.error);
  if ($("showAllPorts")) {
    $("showAllPorts").onchange = () => refreshPorts().catch(console.error);
  }
  $("btnConnect").onclick = () => connect();
  $("btnDisconnect").onclick = () => disconnect();
  $("btnStart").onclick = () => startSession();
  $("btnEnd").onclick = () => endSession();
  $("btnStatus").onclick = async () => {
    try {
      $("sessInfo").textContent = JSON.stringify(await api("/api/status", { method: "POST", body: "{}" }), null, 2);
    } catch (e) {
      alert(e.message);
    }
  };
  $("btnSettime").onclick = async () => {
    try {
      $("sessInfo").textContent = JSON.stringify(await api("/api/settime", { method: "POST", body: "{}" }), null, 2);
    } catch (e) {
      alert(e.message);
    }
  };
}

bind();
refreshPorts().catch(console.error);
refreshProtocols().catch(console.error);
pullState();
openStream();
