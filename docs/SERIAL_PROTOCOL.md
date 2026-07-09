# Host ↔ Station Serial Protocol

**Version:** 0.1  
**Status:** Draft (contract locked before firmware/host implementation)  
**Last Updated:** 2026-07-09

The contract between the Station firmware and the host-side web tool. This is
the **setup, control, and log-streaming** channel — it carries protocol
loading, session start/stop, time injection, and the live CSV row stream plus
LittleFS recovery. It does **not** carry the RF protocol between the boards
(that is `PKT_*` over WiFi/ESP-NOW — see [SPEC §5](SPEC.md)).

Designed schema-first, like the RF packet (`protocol.h`) and the log schema
(SPEC §4). See ADR-004 for the host-tool rationale.

---

## 1. Physical Layer

- USB-CDC serial, **115200 baud, 8N1** (configurable in firmware).
- Byte stream in each direction; lines are `\n`-terminated, UTF-8.
- **No hardware flow control** (RTS/CTS not exposed on most ESP32 dev-board
  USB-CDC). The live `>` stream is best-effort; LittleFS is the durable
  fallback (recovered via `fetch_log` after a session — see §5).
- Max control line length: **4 KB** (protocols are small; chunk over multiple
  `load_protocol` calls later if ever needed). Log rows are ~100 bytes.

---

## 2. Logical Channels (multiplexed by line prefix)

USB-serial is one byte stream in each direction. Two logical channels are
distinguished by the **first byte of each line**:

| Prefix | Channel | Direction | Content |
|--------|---------|-----------|---------|
| `#` | banner | Station→host | Human-readable boot/info lines; host may display or ignore |
| `$` | control | bidirectional | JSON commands (host→Station) and events (Station→host), one object per line |
| `>` | log | Station→host | CSV rows in the locked schema (SPEC §4), streamed live as captured |

Demultiplexing is trivial: the first byte of the line selects the channel.
`grep '^>'` pulls just the log; `grep '^\$'` pulls just the control chatter.
This prefix-per-line pattern is the same one NMEA 0183 uses for the same
reason (one serial line, multiple traffic types).

Lines not starting with one of `#`, `$`, `>` are **errors** and the host should
log a warning and ignore the line. Station guarantees it only emits prefixed lines.

---

## 3. Boot Sequence (Station→host)

On boot, Station emits a banner then a time prompt, then enters Quick-Check
mode (beaconing, no log) regardless of host presence:

```
# ESP32AntTest Station fw 0.3.0
# MAC: AA:BB:CC:DD:EE:FF
# Built: 2026-07-09
$ {"evt":"time_prompt"}
```

The host connecting and sending `start_session` (after `settime` and
`load_protocol`) is what transitions Station to a logged session. Quick-Check
is the default state the host session *overlays*, not a host command.

---

## 4. Control Channel

### 4.1 Commands (host→Station, `$` + JSON)

Every command is a single JSON object on one `$`-prefixed line. Unknown
commands or malformed JSON produce an `{"evt":"error","reason":"..."}`
response.

| `cmd` | Params | Effect | Station replies |
|---|---|---|---|
| `hello` | — | handshake / liveness probe | `{"evt":"hello","board":"station","fw":"0.3.0","mac":"AABBCCDDEEFF"}` |
| `settime` | `iso` *or* `epoch` (either accepted) | set wall-clock baseline from `esp_timer` offset | `{"evt":"time_ok","datetime":"2026-07-09T14:30:22"}`; if no time received in the boot window, `{"evt":"time_unset"}` |
| `load_protocol` | `protocol` (full protocol JSON object) | load protocol for next session; Station validates structure | `{"evt":"protocol_loaded","protocol_id":"range_walk_5m"}` or `{"evt":"protocol_error","reason":"..."}` |
| `start_session` | `mode` (`WIFI`/`ESPNOW`), `tx_mob` (dBm), `tx_sta` (dBm) | begin a logged session with the loaded protocol; Station opens LittleFS file + starts streaming `>` rows + forwards the protocol to Mobile over RF (`PKT_PROTOCOL`) | `{"evt":"session_started","session_id":"20260709_..."}`, then `{"evt":"log_opened","file":"20260709_....csv"}` |
| `end_session` | — | close the current session (always available from host, even if Mobile is out of range) | `{"evt":"log_closed","file":"..."}`, then `{"evt":"session_ended","session_id":"..."}` |
| `status` | — | query current state | `{"evt":"status","state":"quickcheck|session|idle","session_id":"...","step_id":N}` |
| `list_logs` | — | list LittleFS session files | `{"evt":"logs","files":["20260709_....csv",...]}` |
| `fetch_log` | `session` (session_id / filename) | stream the durable CSV on the `>` channel, then terminate (see §5) | rows `>...`, then `{"evt":"fetch_done","bytes":12345}` or `{"evt":"fetch_error","reason":"no such file"}` |
| `delete_log` | `session` | delete a LittleFS session file (housekeeping) | `{"evt":"log_deleted","file":"..."}` |

`settime` accepts whichever of `iso` / `epoch` is present in the JSON:

```
$ {"cmd":"settime","iso":"2026-07-09T14:30:22"}
$ {"cmd":"settime","epoch":1752107422}
```

### 4.2 Events (Station→host, `$` + JSON)

Commands produce the reply events in §4.1. Additionally, Station emits these
**unsolicited** events during a session:

| `evt` | Params | When |
|---|---|---|
| `marker` | `step_id`, `seq` | Mobile forwarded a button-press marker (step advance) — lets the host track protocol progress and attribute buffered outage data to the correct step |
| `link_loss` | `step_id` | RF link loss declared (no decode in either direction for `ANT_LOSS_THRESHOLD` seconds) |
| `mob_rejoined` | `step_id` | Mobile came back into range and forwarded its buffered outage data (host may note a gap-fill; the `source=MOB` rows for the outage are interleaved by `step_id`) |
| `error` | `reason` | generic / out-of-sequence command / malformed JSON / unknown command |

---

## 5. Log Channel (`>` + CSV)

Exactly the locked schema (SPEC §4), one row per line, the prefix `>` prepended:

```
>20260709_143022,5,42,847293,2026-07-09T14:30:22,WIFI,17,20,-67,-71,STA,OK
>20260709_143022,5,43,849293,2026-07-09T14:30:24,WIFI,17,20,-89,,MOB,OK
```

- During a live session: streamed as captured, best-effort. Station writes to
  LittleFS in parallel; the LittleFS file is the durable fallback.
- During `fetch_log` (post-session recovery): the durable CSV is streamed the
  same way, then a `{"evt":"fetch_done","bytes":N}` control line terminates
  the fetch. The host knows the fetch is complete when it sees `fetch_done`
  (or `fetch_error`) on the `$` channel.

If the live stream drops (host falls behind, serial glitch), the host recovers
the durable copy with `fetch_log` after the session.

---

## 6. Session Lifecycle

```
1. connect serial        → Station: # banner, $ time_prompt
2. host: $ settime       → Station: $ time_ok
3. host: $ load_protocol → Station: $ protocol_loaded
4. host: $ start_session → Station: $ session_started, $ log_opened
                           Station: forwards protocol to Mobile (RF, PKT_PROTOCOL)
                           Station: streams > rows as beacons are captured
                           Station: $ marker / $ link_loss / $ mob_rejoined as they occur
5. host: $ end_session   → Station: $ log_closed, $ session_ended
                           (or Mobile long-press → Station emits $ session_ended; host sees it)
6. host: $ fetch_log     → Station: > rows..., $ fetch_done   (recovery if needed)
```

A session can be ended from the host (always) or from Mobile (long-press, if
in range). If Mobile is out of range, the host ends the session — Station
closes the log, and Mobile reconciles on return (sees session-ended). In v1
Mobile has no local log, so an out-of-range end just produces a gap in
Station's record for the steps Mobile was unreachable — which is itself data.

---

## 7. Quick-Check vs Session

Station is **always beaconing** after boot (Quick-Check). `start_session` is
what turns on logging + the `>` stream + Mobile protocol execution.
`end_session` stops logging and returns Station to Quick-Check beaconing.
So Quick-Check is the default power-up state the host session *overlays* —
it is not a host command.

---

## 8. Boundaries (out of scope for this protocol)

- **Station→Mobile protocol transfer** happens over RF (`PKT_PROTOCOL`,
  chunked), not over serial. The serial protocol only delivers the protocol
  JSON *to Station*; Station owns forwarding it to Mobile.
- **Host↔Mobile** has no direct serial link — Mobile is USB-only for power.
  All Mobile data reaches the host via Station (live `>` rows + forwarded
  markers/outage data).
- **No binary framing / length-prefix / checksum** on the serial line —
  text/JSON + CSV with line prefixes, by design (see ADR-004 addendum: the
  host tool stays simple and debuggable).
