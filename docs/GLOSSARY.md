# ESP32AntTest — Glossary

Terms used consistently throughout this project's documentation, code, and logs.

---

**Auto Mode**  
A test mode in which the Mobile triggers ping exchanges automatically at a fixed interval (default 5 s) until the user stops recording. Used for time-soak scenarios.

**Board Config**  
A per-board header file (`board_config.h`) containing GPIO pin assignments and hardware-specific constants. Allows the shared firmware to target different ESP32 variants without code changes.

**Burst**  
A rapid sequence of N ping-pong exchanges triggered by a single button press in Manual Mode. Default N = 10. Results are averaged or logged individually within one Run.

**C3 / Mobile Board**  
The ESP32-C3 development module, default hardware for the Mobile role. RISC-V single-core, 2.4 GHz only, sufficient GPIO for OLED + button.

**Double Press**  
A button gesture: two short presses within 400 ms. Used as Back / Cancel in the UI.

**ESP-NOW**  
Espressif's connectionless peer-to-peer 2.4 GHz protocol. Operates without an access point. Provides longer range than standard WiFi at lower data rate (1 Mbps base). Used in Mode B.

**Link Loss**  
A declared state when ANT_LOSS_THRESHOLD consecutive pings receive no response. The current run is closed and an error is logged.

**Long Press**  
A button gesture: button held ≥ 1500 ms. Used as Select / Confirm in the UI.

**Manual Mode**  
A test mode in which each button press triggers one Burst. Used for range-walk scenarios where the user moves the Mobile and captures data at each stop.

**Mobile**  
The handheld board. Runs the OLED display, button UI, and session control logic. Initiates all ping exchanges. Default hardware: ESP32-C3.

**Mode A — WiFi Peer**  
RF mode where Station acts as SoftAP and Mobile connects as STA. No external router. RSSI obtained via native ESP-IDF WiFi API on both sides.

**Mode B — ESP-NOW**  
RF mode where both boards use ESP-NOW protocol. RSSI obtained via promiscuous mode callback (`wifi_promiscuous_pkt_t.rx_ctrl.rssi`) on both sides.

**OLED**  
0.96-inch SSD1306 monochrome display, 128×64 pixels, I2C interface. Mounted on the Mobile board.

**Ping**  
A `PKT_PING` packet sent by Mobile to Station to initiate one measurement exchange.

**Pong**  
A `PKT_PONG` packet sent by Station to Mobile in response to a Ping. Contains Station's locally-measured RSSI and TX power.

**Promiscuous Mode**  
An ESP-IDF WiFi driver mode in which the radio receives all 802.11 frames on the current channel, not just those addressed to the device. Used to extract RSSI from the radio tap header of received packets in Mode B.

**RSSI (Received Signal Strength Indicator)**  
A measure of the power level of a received radio signal, reported in dBm (negative values; closer to 0 is stronger). In this project, always refers to the signal received *at a given board from the other board*. The two RSSI values in a sample are therefore independent and directionally distinct.

**RSSI_MOB**  
RSSI of Station's packet as measured at the Mobile board. Represents the downlink (Station→Mobile) signal quality.

**RSSI_STA**  
RSSI of Mobile's packet as measured at the Station board. Represents the uplink (Mobile→Station) signal quality.

**Run**  
A contiguous sequence of ping-pong exchanges at one position or configuration, logged as a group. In Manual Mode, each button press is one Run. In Auto Mode, one start-to-stop sequence is one Run.

**Session**  
A named collection of Runs sharing a common setup (antenna configuration, board pair, test objective). Identified by a timestamp-based ID. Stored as one CSV file on Station's LittleFS.

**Short Press**  
A button gesture: button pressed and released in < 500 ms. Used for Scroll / Next in the UI.

**SoftAP**  
Software Access Point. One board acts as an access point in firmware. In Mode A, Station is the SoftAP; Mobile connects to it as a WiFi client (STA). No wired uplink is needed.

**Station**  
The stationary board. Responds to pings, measures uplink RSSI, logs data to LittleFS and serial. Default hardware: ESP32-WROOM-32.

**STA (WiFi Station mode)**  
Standard WiFi client mode. In Mode A, Mobile is the STA, connecting to Station's SoftAP.

**Time Soak**  
A test scenario in which the two boards are placed at a fixed location and Auto Mode records signal quality over an extended period (minutes to hours) to observe temporal variation from interference, multipath, and environmental changes.

**TX Power**  
Transmit power of the WiFi/ESP-NOW radio, set via `esp_wifi_set_max_tx_power()`. Expressed in dBm. User-selectable from the Mobile menu: 2, 10, 17, or 20 dBm. Both boards' TX power values are included in every logged record.

**WROOM / Station Board**  
The ESP32-WROOM-32 development module, default hardware for the Station role. Dual-core Xtensa, PCB trace antenna, USB-serial for logging.
