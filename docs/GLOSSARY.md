# ESP32AntTest — Glossary

Terms used consistently throughout this project's documentation, code, and logs.

---

**Beacon**  
A `PKT_BEACON` packet transmitted autonomously by each board at a fixed rate
(default 5 Hz). Each beacon piggybacks `rssi_local` — the sender's freshest
RSSI measurement of the peer — so a single received beacon yields both
directional RSSI values. Beacons are the only RF sampling traffic; there is
no request-response / ping-pong exchange (see ADR-004).

**Board Config**  
A per-board header file (`board_config.h`) containing GPIO pin assignments
and hardware-specific constants. Allows the shared firmware to target
different ESP32 variants without code changes.

**C3 Board**  
The ESP32-C3 development module. RISC-V single-core, 2.4 GHz only, ceramic PCB antenna (onboard), sufficient GPIO for OLED + button. Can fill either role (see Configurations in [HARDWARE.md](HARDWARE.md)).

**Double Press**  
A button gesture: two short presses within 400 ms. Used as Back / Cancel in the UI.

**ESP-NOW**  
Espressif's connectionless peer-to-peer 2.4 GHz protocol. Operates without an access point. Provides longer range than standard WiFi at lower data rate (1 Mbps base). Used in Mode B.

**Link Loss**  
A declared state after `ANT_LOSS_THRESHOLD` consecutive seconds with no
decoded beacon in either direction. The current run is closed. Under beacon
mode there is no per-row `TIMEOUT` — loss is inferred from sample count vs.
expected (`beacon_hz × step_duration`). See [SPEC §5.3](SPEC.md).

**Long Press**  
A button gesture: button held ≥ 1500 ms. Used as Select / Confirm in the UI.

**Mobile**  
The handheld board. Runs the OLED display, button UI, and session control
logic. Under beacon mode both boards transmit and receive autonomously;
Mobile additionally buffers outage data in RAM and forwards it to Station on
reconnect. Default hardware: ESP32-C3 (Config A); configurable, see
[HARDWARE.md](HARDWARE.md).

**Mode A — WiFi Peer**  
RF mode where Station acts as SoftAP and Mobile connects as STA. No external router. Beacons are UDP datagrams; RSSI is captured per-beacon via promiscuous callback on both boards (see ADR-001 addendum).

**Mode B — ESP-NOW**  
RF mode where both boards use ESP-NOW protocol. Beacons are native ESP-NOW packets; RSSI is captured per-beacon via promiscuous callback (`wifi_promiscuous_pkt_t.rx_ctrl.rssi`) on both boards.

**OLED**  
0.96-inch SSD1306 monochrome display, 128×64 pixels, I2C interface. Mounted on the Mobile board.

**Promiscuous Mode**  
An ESP-IDF WiFi driver mode in which the radio receives all 802.11 frames on the current channel, not just those addressed to the device. Used to extract per-beacon RSSI via `wifi_promiscuous_pkt_t.rx_ctrl.rssi`, filtered by peer MAC, on both boards and in both RF modes (see ADR-001 addendum).

**RSSI (Received Signal Strength Indicator)**  
A measure of the power level of a received radio signal, reported in dBm (negative values; closer to 0 is stronger). In this project, always refers to the signal received *at a given board from the other board*. The two RSSI values in a sample are therefore independent and directionally distinct.

**RSSI_MOB**  
RSSI of Station's beacon as measured at the Mobile board. Represents the downlink (Station→Mobile) signal quality.

**RSSI_STA**  
RSSI of Mobile's beacon as measured at the Station board. Represents the uplink (Mobile→Station) signal quality.

**Run**  
A contiguous sequence of beacon samples at one position or configuration,
logged as a group. Maps 1:1 to a protocol step (join key `step_id`). See
[SPEC §3.2](SPEC.md).

**Session**  
A named collection of Runs sharing a common setup (antenna configuration, board pair, test objective). Identified by a timestamp-based ID. Stored as one CSV file on Station's LittleFS.

**Short Press**  
A button gesture: button pressed and released in < 500 ms. Used for Scroll / Next in the UI.

**SoftAP**  
Software Access Point. One board acts as an access point in firmware. In Mode A, Station is the SoftAP; Mobile connects to it as a WiFi client (STA). No wired uplink is needed.

**Station**  
The stationary board. Logs every beacon it decodes from Mobile (both RSSI
values via piggyback), streams each row over serial to the host, and mirrors
it to LittleFS as the durable fallback. Default hardware: ESP32-WROOM-32
(Config A); configurable, see [HARDWARE.md](HARDWARE.md).

**STA (WiFi Station mode)**  
Standard WiFi client mode. In Mode A, Mobile is the STA, connecting to Station's SoftAP.

**Time Soak**  
A host-guided test scenario: boards stay at a fixed placement while a session
logs continuous beacons for a planned interval (protocol `soak` with
`duration_s`, or a long `free` step). Value is on the host — RSSI-vs-time
(and related) plots after the session, not a Mobile-local "Auto" sampler.
See [SPEC §3.2–3.3](SPEC.md).

**TX Power**  
Transmit power of the WiFi/ESP-NOW radio, set via `esp_wifi_set_max_tx_power()`. Expressed in dBm. User-selectable from the Mobile menu: 2, 10, 17, or 20 dBm. Both boards' TX power values are included in every logged record.

**WROOM Board**  
The ESP32-WROOM-32 development module. Dual-core Xtensa, FPC antenna adhered to the PCB, USB-serial for logging. Can fill either role (see Configurations in [HARDWARE.md](HARDWARE.md)).
