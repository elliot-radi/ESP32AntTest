/* ESP32AntTest — Station RF layer.
 * SoftAP (Mode A) + ESP-NOW (Mode B) beacons, promiscuous RSSI, logging.
 */
#include "rf.h"
#include "config.h"
#include "session.h"
#include "logger.h"
#include "serial.h"
#include "protocol.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "esp_now.h"
#include "esp_wifi_types.h"
#include "nvs_flash.h"
#include "cJSON.h"

static const char *TAG = "rf";

/* ---- Max stashed protocol JSON (SERIAL_PROTOCOL.md: 4 KB control line) ---- */
#define ANT_PROTOCOL_JSON_MAX  4096

/* SoftAP static IP from ANT_STATION_IP (default 192.168.26.1) — NOT the
 * ESP-IDF 192.168.4.1 default. Subnet /24. */
#define SOFTAP_NETMASK "255.255.255.0"

/* ---- shared RF state (mutex-protected for any multi-writer path) ---- */
static SemaphoreHandle_t s_mu;

static ant_mode_t s_mode          = ANT_MODE_WIFI;
static bool       s_running       = false;
static bool       s_logging       = false;   /* true while a session is open */
static int8_t     s_tx_dbm        = ANT_DEFAULT_TX_POWER;
static int8_t     s_rssi_local    = -128;    /* last measured peer RSSI; piggyback */
static uint32_t   s_seq           = 0;
static uint8_t    s_peer_mac[6]   = {0};
static bool       s_peer_known    = false;
static uint8_t    s_peer_ip[4]    = {0};     /* last associated STA IP (Mode A) */
static bool       s_peer_ip_known = false;

static char      *s_protocol_json = NULL;    /* heap copy; may be NULL */
static bool       s_protocol_fwd_pending = false;
/* Session RF commanded by host and carried on PKT_PROTOCOL so Mobile can
 * apply the same setup before (mode switch) / without relying on SoftAP.
 * Station's own TX is s_tx_dbm; mode on PROTOCOL is for Mobile. */
static int8_t     s_session_tx_mob = ANT_DEFAULT_TX_POWER;
static ant_mode_t s_session_mode   = ANT_MODE_WIFI;

/* UDP socket for Mode A beacons (bound to ANT_UDP_PORT). */
static int        s_udp_sock      = -1;

/* Last decoded-any-direction timestamps for link-loss (seconds, boot). */
static int64_t    s_last_rx_s     = 0;
static bool       s_link_lost     = false;

/* ---- helpers ---- */

static void lock(void)   { if (s_mu) xSemaphoreTake(s_mu, portMAX_DELAY); }
static void unlock(void) { if (s_mu) xSemaphoreGive(s_mu); }

static uint32_t session_id_wire(void)
{
    /* Lower 32 bits of a packed YYYYMMDD_HHMMSS if available; else 0.
     * SPEC §5: "lower 32 bits of session timestamp". We parse digits. */
    const station_session_t *s = session_get();
    if (!s) return 0;
    /* session_id like "20260709_143022" — strip '_' and take low 32 of the number. */
    char digits[16];
    int di = 0;
    for (const char *p = s->session_id; *p && di < (int)sizeof(digits) - 1; p++) {
        if (*p >= '0' && *p <= '9') digits[di++] = *p;
    }
    digits[di] = 0;
    /* Use last 9 digits to fit in 32-bit unsigned less lagely. */
    const char *use = digits;
    if (di > 9) use = digits + (di - 9);
    return (uint32_t)strtoul(use, NULL, 10);
}

static uint16_t active_step_id(void)
{
    const station_session_t *s = session_get();
    return s ? s->step_id : 0;
}

static void note_rx_now(void)
{
    s_last_rx_s = esp_timer_get_time() / 1000000;
    if (s_link_lost) {
        s_link_lost = false;
        cJSON *j = cJSON_CreateObject();
        cJSON_AddStringToObject(j, "evt", "mob_rejoined");
        cJSON_AddNumberToObject(j, "step_id", active_step_id());
        serial_emit_evt_obj(j);
    }
}

/* ---- log a decoded Mobile beacon (Station RX) ---- */

static void log_beacon_row(const ant_packet_t *pkt, int8_t rssi_sta, bool mob_outage)
{
    if (!s_logging) return;
    const station_session_t *s = session_get();
    if (!s) return;

    /* MOB outage replay: rssi_mob is what Mobile measured during the outage;
     * rssi_sta is empty (we never decoded the uplink at the time — SPEC §4).
     * Do NOT use the wire RSSI of the late forward as rssi_sta. */
    log_row_t row = {
        .session_id   = s->session_id,
        .step_id      = pkt->step_id ? pkt->step_id : s->step_id,
        .seq          = pkt->seq,
        .timestamp_ms = (uint64_t)(esp_timer_get_time() / 1000),
        .mode         = s->mode,
        .tx_mob       = pkt->tx_power,
        .tx_sta       = ant_rf_get_tx_power_dbm(),
        .rssi_mob     = pkt->rssi_local,
        .rssi_sta     = mob_outage ? (int8_t)-128 : rssi_sta,
        .source       = mob_outage ? LOG_SRC_MOB : LOG_SRC_STA,
        .status       = LOG_STATUS_OK,
    };
    logger_emit_row(&row);
}

/* Handle an inbound ant_packet from Mobile (any transport). */
static void handle_peer_packet(const ant_packet_t *pkt, int8_t rssi,
                               const uint8_t src_mac[6])
{
    lock();
    if (src_mac) {
        memcpy(s_peer_mac, src_mac, 6);
        s_peer_known = true;
    }
    if (rssi != (int8_t)-128) {
        s_rssi_local = rssi;
    }
    note_rx_now();
    unlock();

    switch (pkt->type) {
    case PKT_BEACON: {
        bool mob = (pkt->reserved[0] & ANT_RSV0_MOB_OUTAGE) != 0;
        log_beacon_row(pkt, rssi, mob);
        if (mob) {
            /* Late drain of outage backlog — surface as a control nudge so the
             * host can note gap-fill (SERIAL_PROTOCOL.md §4.2 mob_rejoined is
             * for the link event; individual rows ride the > channel). */
            ESP_LOGI(TAG, "MOB outage row seq=%u step=%u rssi_mob=%d",
                     (unsigned)pkt->seq, pkt->step_id, (int)pkt->rssi_local);
        } else if (pkt->step_id) {
            /* Stay in lockstep with Mobile's active step on live beacons. */
            session_set_step(pkt->step_id);
        }
        break;
    }

    case PKT_MARKER: {
        /* Button-press / step advance from Mobile (live or post-outage). */
        session_set_step(pkt->step_id);
        cJSON *j = cJSON_CreateObject();
        cJSON_AddStringToObject(j, "evt", "marker");
        cJSON_AddNumberToObject(j, "step_id", pkt->step_id);
        cJSON_AddNumberToObject(j, "seq", pkt->seq);
        serial_emit_evt_obj(j);
        ESP_LOGI(TAG, "marker step_id=%u seq=%u", pkt->step_id, (unsigned)pkt->seq);
        break;
    }

    case PKT_PROTOCOL:
        /* Mobile does not send protocol chunks to Station in v1. */
        break;

    case PKT_MODE_ACK:
        ESP_LOGI(TAG, "mode_ack from Mobile");
        break;

    default:
        break;
    }

    /* If we were waiting to forward a protocol and now know the peer, try. */
    if (s_protocol_fwd_pending) {
        ant_rf_forward_protocol();
    }
}

/* ---- promiscuous callback: per-frame RSSI filtered by peer MAC ----
 * Runs in WiFi task context — keep it short: copy rssi, queue nothing free.
 * Actual packet payload is decoded on the UDP / ESP-NOW path; here we only
 * refresh s_rssi_local when the source MAC matches the known peer (or any
 * non-broadcast SA once a peer is known). For peer discovery before MAC is
 * known, the SoftAP STA-list / ESP-NOW recv CB provides the MAC. */

static void IRAM_ATTR promiscuous_rx_cb(void *buf, wifi_promiscuous_pkt_type_t type)
{
    if (type != WIFI_PKT_DATA && type != WIFI_PKT_MGMT) return;
    const wifi_promiscuous_pkt_t *ppkt = (const wifi_promiscuous_pkt_t *)buf;
    const uint8_t *payload = ppkt->payload;
    /* 802.11 header: addr2 (SA) is at offset 10 for most data frames. */
    if (ppkt->rx_ctrl.sig_len < 16) return;
    const uint8_t *sa = payload + 10;

    /* Only refresh RSSI for our peer once known; before that, ignore. */
    if (!s_peer_known) return;
    if (memcmp(sa, s_peer_mac, 6) != 0) return;

    s_rssi_local = ppkt->rx_ctrl.rssi;
}

/* ---- UDP RX task (Mode A) ---- */

static void udp_rx_task(void *arg)
{
    uint8_t buf[128];
    struct sockaddr_in src;
    socklen_t slen = sizeof(src);
    while (1) {
        if (s_udp_sock < 0 || s_mode != ANT_MODE_WIFI) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        int n = recvfrom(s_udp_sock, buf, sizeof(buf), 0,
                         (struct sockaddr *)&src, &slen);
        if (n < (int)sizeof(ant_packet_t)) {
            if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
                vTaskDelay(pdMS_TO_TICKS(10));
            }
            continue;
        }
        ant_packet_t pkt;
        if (ant_packet_decode(buf, n, &pkt) != 0) continue;

        /* Learn peer IP from the datagram source. */
        lock();
        memcpy(s_peer_ip, &src.sin_addr.s_addr, 4);
        s_peer_ip_known = true;
        unlock();

        /* RSSI from promiscuous cache (may be -128 if not yet seen). */
        int8_t rssi = s_rssi_local;

        /* Peer MAC: SoftAP STA list if not yet known. */
        uint8_t mac[6] = {0};
        bool have_mac = s_peer_known;
        if (have_mac) {
            memcpy(mac, s_peer_mac, 6);
        } else {
            wifi_sta_list_t list;
            if (esp_wifi_ap_get_sta_list(&list) == ESP_OK && list.num > 0) {
                memcpy(mac, list.sta[0].mac, 6);
                have_mac = true;
                /* Prefer per-STA RSSI from the AP list if promiscuous
                 * hasn't fired yet (smoothed, but better than empty). */
                if (rssi == (int8_t)-128) rssi = list.sta[0].rssi;
            }
        }

        handle_peer_packet(&pkt, rssi, have_mac ? mac : NULL);
    }
}

/* ---- ESP-NOW RX callback ---- */

static void espnow_recv_cb(const esp_now_recv_info_t *info,
                           const uint8_t *data, int len)
{
    if (!info || !data || len < (int)sizeof(ant_packet_t)) return;
    ant_packet_t pkt;
    if (ant_packet_decode(data, len, &pkt) != 0) return;

    int8_t rssi = (int8_t)-128;
    if (info->rx_ctrl) rssi = info->rx_ctrl->rssi;
    /* Prefer promiscuous cache if fresher look, else rx_ctrl. */
    if (s_rssi_local != (int8_t)-128) rssi = s_rssi_local;
    else if (rssi != (int8_t)-128) s_rssi_local = rssi;

    handle_peer_packet(&pkt, rssi, info->src_addr);
}

/* ---- build + send one beacon ---- */

static void send_beacon(void)
{
    ant_packet_t pkt = {0};
    pkt.magic[0]   = ANT_MAGIC_0;
    pkt.magic[1]   = ANT_MAGIC_1;
    pkt.version    = ANT_PROTO_VER;
    pkt.type       = PKT_BEACON;
    pkt.seq        = ++s_seq;
    pkt.session_id = session_id_wire();
    pkt.step_id    = active_step_id();
    pkt.rssi_local = s_rssi_local;
    pkt.tx_power   = s_tx_dbm;

    uint8_t wire[sizeof(ant_packet_t)];
    ant_packet_encode(&pkt, wire);

    if (s_mode == ANT_MODE_WIFI) {
        if (s_udp_sock < 0) return;
        struct sockaddr_in dest = {0};
        dest.sin_family = AF_INET;
        dest.sin_port   = htons(ANT_UDP_PORT);
        if (s_peer_ip_known) {
            memcpy(&dest.sin_addr.s_addr, s_peer_ip, 4);
        } else {
            /* Broadcast on the SoftAP subnet until we learn the STA IP. */
            char bcast[16];
            /* Derive broadcast from ANT_STATION_IP: a.b.c.255 */
            unsigned a, b, c, d;
            if (sscanf(ANT_STATION_IP, "%u.%u.%u.%u", &a, &b, &c, &d) == 4) {
                snprintf(bcast, sizeof(bcast), "%u.%u.%u.255", a, b, c);
                dest.sin_addr.s_addr = inet_addr(bcast);
            } else {
                dest.sin_addr.s_addr = inet_addr("255.255.255.255");
            }
        }
        sendto(s_udp_sock, wire, sizeof(wire), 0,
               (struct sockaddr *)&dest, sizeof(dest));
    } else {
        /* ESP-NOW: unicast if peer known, else broadcast. */
        uint8_t bcast[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
        const uint8_t *dst = s_peer_known ? s_peer_mac : bcast;
        esp_now_send(dst, wire, sizeof(wire));
    }
}

/* Chunked PKT_PROTOCOL forward.
 * reserved[0] = chunk index, reserved[1] = total chunks, seq = byte offset.
 * Payload of each chunk is not in ant_packet_t (fixed 20 B) — SPEC says
 * "chunked protocol transfer". For v1 we put a small hint only and also
 * ship the full JSON as successive UDP datagrams: a 4-byte magic header
 * `PRxx` framing on a side channel would break the ant_packet_t rule.
 *
 * Practical v1 approach matching the 20-byte packet: send N PKT_PROTOCOL
 * packets where reserved[0]=idx, reserved[1]=total, and the JSON is NOT
 * in the packet — Mobile is expected to have received it via a longer
 * UDP datagram that wraps the packet header + JSON body.
 *
 * Extended frame (Mode A / ESP-NOW payload):
 *   [ant_packet_t 20 B][uint16_t total_len LE][uint16_t offset LE][chunk bytes...]
 *   where pkt.type = PKT_PROTOCOL, reserved[0]=chunk_idx, reserved[1]=n_chunks.
 */
#define PROTO_CHUNK_DATA  200

static void send_protocol_chunks(void)
{
    /* Always emit at least one PKT_PROTOCOL frame so Mobile can apply the
     * session's tx_mob / mode even when JSON forward races or is empty
     * (session end mode tear-down). Empty payload = total_len 0. */
    const char *json = (s_protocol_json && s_protocol_json[0]) ? s_protocol_json : "";
    uint16_t total = (uint16_t)strlen(json);
    uint16_t offset = 0;
    uint8_t n_chunks = (total == 0)
        ? 1
        : (uint8_t)((total + PROTO_CHUNK_DATA - 1) / PROTO_CHUNK_DATA);
    if (n_chunks == 0) n_chunks = 1;

    /* Host-commanded Mobile TX + mode for this session — NOT Station's own. */
    int8_t tx_mob_cmd = s_session_tx_mob;
    ant_mode_t mode_cmd = s_session_mode;
    const station_session_t *sess = session_get();
    if (sess) {
        tx_mob_cmd = sess->tx_mob;
        mode_cmd   = sess->mode;
    }

    for (uint8_t idx = 0; idx < n_chunks; idx++) {
        uint16_t take = (total > offset) ? (uint16_t)(total - offset) : 0;
        if (take > PROTO_CHUNK_DATA) take = PROTO_CHUNK_DATA;

        ant_packet_t pkt = {0};
        pkt.magic[0] = ANT_MAGIC_0;
        pkt.magic[1] = ANT_MAGIC_1;
        pkt.version  = ANT_PROTO_VER;
        pkt.type     = PKT_PROTOCOL;
        pkt.seq      = offset;
        pkt.session_id = session_id_wire();
        pkt.step_id  = (uint16_t)mode_cmd; /* commanded Mobile mode */
        pkt.rssi_local = s_rssi_local;
        pkt.tx_power = tx_mob_cmd;   /* commanded Mobile TX (dBm) */
        pkt.reserved[0] = idx;
        pkt.reserved[1] = n_chunks;
        pkt.reserved[2] = (uint8_t)(total & 0xFF);
        pkt.reserved[3] = (uint8_t)((total >> 8) & 0xFF);

        uint8_t frame[sizeof(ant_packet_t) + 4 + PROTO_CHUNK_DATA];
        ant_packet_encode(&pkt, frame);
        /* uint16 total_len LE + uint16 offset LE + data */
        frame[20] = (uint8_t)(total & 0xFF);
        frame[21] = (uint8_t)((total >> 8) & 0xFF);
        frame[22] = (uint8_t)(offset & 0xFF);
        frame[23] = (uint8_t)((offset >> 8) & 0xFF);
        memcpy(frame + 24, json + offset, take);
        int flen = 24 + take;

        if (s_mode == ANT_MODE_WIFI) {
            if (s_udp_sock < 0) return;
            struct sockaddr_in dest = {0};
            dest.sin_family = AF_INET;
            dest.sin_port   = htons(ANT_UDP_PORT);
            if (s_peer_ip_known) {
                memcpy(&dest.sin_addr.s_addr, s_peer_ip, 4);
            } else {
                unsigned a, b, c, d;
                char bcast[16];
                if (sscanf(ANT_STATION_IP, "%u.%u.%u.%u", &a, &b, &c, &d) == 4)
                    snprintf(bcast, sizeof(bcast), "%u.%u.%u.255", a, b, c);
                else
                    snprintf(bcast, sizeof(bcast), "255.255.255.255");
                dest.sin_addr.s_addr = inet_addr(bcast);
            }
            sendto(s_udp_sock, frame, flen, 0,
                   (struct sockaddr *)&dest, sizeof(dest));
        } else {
            uint8_t bcast[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
            const uint8_t *dst = s_peer_known ? s_peer_mac : bcast;
            /* ESP-NOW max ~250 B; frame fits. */
            esp_now_send(dst, frame, flen);
        }
        offset += take;
        vTaskDelay(pdMS_TO_TICKS(20));   /* gentle pacing */
    }
    ESP_LOGI(TAG, "protocol forward: %u bytes in %u chunks", total, n_chunks);
}

void ant_rf_forward_protocol(void)
{
    /* Always forward session setup (tx_mob + optional JSON). Mark pending
     * when no STA peer yet so the SoftAP join path retries. */
    s_protocol_fwd_pending = !s_peer_known;
    send_protocol_chunks();
    if (s_peer_known) s_protocol_fwd_pending = false;
}

/* ---- beacon + link-loss task ---- */

static void beacon_task(void *arg)
{
    const TickType_t period = pdMS_TO_TICKS(1000 / ANT_BEACON_HZ);
    int64_t last_check_s = 0;
    while (1) {
        if (s_running) {
            send_beacon();

            /* Link-loss check once per second. */
            int64_t now_s = esp_timer_get_time() / 1000000;
            if (now_s != last_check_s) {
                last_check_s = now_s;
                if (s_last_rx_s > 0 &&
                    (now_s - s_last_rx_s) >= ANT_LOSS_THRESHOLD &&
                    !s_link_lost && s_logging) {
                    s_link_lost = true;
                    cJSON *j = cJSON_CreateObject();
                    cJSON_AddStringToObject(j, "evt", "link_loss");
                    cJSON_AddNumberToObject(j, "step_id", active_step_id());
                    serial_emit_evt_obj(j);
                    ESP_LOGW(TAG, "link_loss declared (no RX for %d s)",
                             ANT_LOSS_THRESHOLD);
                }
            }
        }
        vTaskDelay(period);
    }
}

/* ---- SoftAP bring-up ---- */

static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STACONNECTED) {
        wifi_event_ap_staconnected_t *e = (wifi_event_ap_staconnected_t *)data;
        lock();
        memcpy(s_peer_mac, e->mac, 6);
        s_peer_known = true;
        /* Fresh association — forget prior STA IP so we broadcast until we
         * learn the new lease (handles SoftAP DHCP re-issue on rejoin). */
        s_peer_ip_known = false;
        unlock();
        ESP_LOGI(TAG, "STA joined: %02X:%02X:%02X:%02X:%02X:%02X",
                 e->mac[0], e->mac[1], e->mac[2],
                 e->mac[3], e->mac[4], e->mac[5]);
        /* Re-forward whenever a session is active (or still pending from
         * start_session before first join). Fixes Mobile boot/rejoin after
         * the one-shot forward at session start. */
        if (s_logging || s_protocol_fwd_pending) {
            ESP_LOGI(TAG, "re-forward protocol on STA join (logging=%d pending=%d)",
                     (int)s_logging, (int)s_protocol_fwd_pending);
            ant_rf_forward_protocol();
        }
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STADISCONNECTED) {
        ESP_LOGI(TAG, "STA left");
        s_peer_ip_known = false;
    }
}

static int open_udp_socket(void)
{
    if (s_udp_sock >= 0) {
        close(s_udp_sock);
        s_udp_sock = -1;
    }
    int s = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (s < 0) {
        ESP_LOGE(TAG, "UDP socket: %s", strerror(errno));
        return -1;
    }
    int yes = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    setsockopt(s, SOL_SOCKET, SO_BROADCAST, &yes, sizeof(yes));

    /* Non-blocking so the RX task can poll alongside mode checks. */
    struct timeval tv = { .tv_sec = 0, .tv_usec = 200000 };
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(ANT_UDP_PORT);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(s, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "UDP bind: %s", strerror(errno));
        close(s);
        return -1;
    }
    s_udp_sock = s;
    ESP_LOGI(TAG, "UDP bound on %d", ANT_UDP_PORT);
    return 0;
}

static int start_softap(void)
{
    wifi_config_t cfg = {0};
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    /* SSID: AntTest-<MAC4> (last 2 bytes as 4 hex chars) — SPEC §3.1 */
    snprintf((char *)cfg.ap.ssid, sizeof(cfg.ap.ssid),
             "%s-%02X%02X", ANT_SOFTAP_SSID_PREFIX, mac[4], mac[5]);
    cfg.ap.ssid_len = strlen((char *)cfg.ap.ssid);
    cfg.ap.channel = ANT_WIFI_CHANNEL;
    cfg.ap.authmode = WIFI_AUTH_OPEN;
    cfg.ap.max_connection = 4;
    cfg.ap.ssid_hidden = 0;
    if (ANT_SOFTAP_PASS[0] != '\0') {
        strncpy((char *)cfg.ap.password, ANT_SOFTAP_PASS, sizeof(cfg.ap.password) - 1);
        cfg.ap.authmode = WIFI_AUTH_WPA2_PSK;
    }

    /* Set SoftAP static IP BEFORE wifi_start so DHCPS leases from ANT_STATION_IP. */
    esp_netif_t *ap_netif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    if (ap_netif) {
        ESP_ERROR_CHECK(esp_netif_dhcps_stop(ap_netif));
        esp_netif_ip_info_t ip_info;
        memset(&ip_info, 0, sizeof(ip_info));
        ip_info.ip.addr = inet_addr(ANT_STATION_IP);
        ip_info.gw.addr = inet_addr(ANT_STATION_IP);
        ip_info.netmask.addr = inet_addr(SOFTAP_NETMASK);
        ESP_ERROR_CHECK(esp_netif_set_ip_info(ap_netif, &ip_info));
    }

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    if (ap_netif) {
        ESP_ERROR_CHECK(esp_netif_dhcps_start(ap_netif));
    }

    ESP_LOGI(TAG, "SoftAP up: SSID=%s ch=%d ip=%s",
             cfg.ap.ssid, ANT_WIFI_CHANNEL, ANT_STATION_IP);
    return 0;
}

static int start_espnow(void)
{
    /* ESP-NOW needs WiFi started. Use APSTA so SoftAP can stay for
     * recovery, but primary path is ESP-NOW on the same channel. Spec
     * says Mode B is peer ESP-NOW — AP mode alone is enough on Station. */
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    wifi_config_t cfg = {0};
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    snprintf((char *)cfg.ap.ssid, sizeof(cfg.ap.ssid),
             "%s-%02X%02X", ANT_SOFTAP_SSID_PREFIX, mac[4], mac[5]);
    cfg.ap.ssid_len = strlen((char *)cfg.ap.ssid);
    cfg.ap.channel = ANT_WIFI_CHANNEL;
    cfg.ap.authmode = WIFI_AUTH_OPEN;
    cfg.ap.max_connection = 4;
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &cfg));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_channel(ANT_WIFI_CHANNEL, WIFI_SECOND_CHAN_NONE));

    esp_err_t err = esp_now_init();
    if (err != ESP_OK && err != ESP_ERR_ESPNOW_EXIST) {
        ESP_LOGE(TAG, "esp_now_init: %s", esp_err_to_name(err));
        return -1;
    }
    ESP_ERROR_CHECK(esp_now_register_recv_cb(espnow_recv_cb));

    /* Add broadcast peer so we can shout before knowing Mobile's MAC. */
    esp_now_peer_info_t peer = {0};
    memset(peer.peer_addr, 0xff, 6);
    peer.channel = ANT_WIFI_CHANNEL;
    peer.ifidx   = WIFI_IF_AP;
    peer.encrypt = false;
    esp_now_del_peer(peer.peer_addr);  /* ok if missing */
    err = esp_now_add_peer(&peer);
    if (err != ESP_OK && err != ESP_ERR_ESPNOW_EXIST) {
        ESP_LOGW(TAG, "add broadcast peer: %s", esp_err_to_name(err));
    }

    if (s_peer_known) {
        memcpy(peer.peer_addr, s_peer_mac, 6);
        esp_now_del_peer(peer.peer_addr);
        err = esp_now_add_peer(&peer);
        if (err != ESP_OK && err != ESP_ERR_ESPNOW_EXIST)
            ESP_LOGW(TAG, "add unicast peer: %s", esp_err_to_name(err));
    }

    ESP_LOGI(TAG, "ESP-NOW up on ch %d", ANT_WIFI_CHANNEL);
    return 0;
}

static void stop_transports(void)
{
    if (s_udp_sock >= 0) {
        close(s_udp_sock);
        s_udp_sock = -1;
    }
    esp_now_deinit();   /* safe even if not inited */
    esp_wifi_stop();
}

/* ---- public API ---- */

void ant_rf_init(void)
{
    s_mu = xSemaphoreCreateMutex();

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    } else {
        ESP_ERROR_CHECK(err);
    }

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t wcfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wcfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));

    s_tx_dbm = ANT_DEFAULT_TX_POWER;
    s_mode   = ANT_MODE_WIFI;
    s_running = false;
    s_logging = false;
}

void ant_rf_start(void)
{
    if (s_running) return;

    start_softap();
    open_udp_socket();

    /* Promiscuous RX for per-beacon RSSI (ADR-001 addendum). */
    ESP_ERROR_CHECK(esp_wifi_set_promiscuous(true));
    ESP_ERROR_CHECK(esp_wifi_set_promiscuous_rx_cb(promiscuous_rx_cb));

    ant_rf_set_tx_power(s_tx_dbm);

    s_running = true;
    s_mode = ANT_MODE_WIFI;
    s_last_rx_s = esp_timer_get_time() / 1000000;  /* don't immediately link-loss */

    /* UDP RX + beacon tasks. Stack: UDP is light; 8 KB is enough. */
    xTaskCreate(udp_rx_task,  "udp_rx",  8192, NULL, 5, NULL);
    xTaskCreate(beacon_task,  "beacon",  8192, NULL, 5, NULL);

    ESP_LOGI(TAG, "RF started (Quick-Check / Mode A WiFi)");
}

int ant_rf_set_mode(ant_mode_t mode)
{
    if (!s_running) {
        s_mode = mode;
        return 0;
    }
    if (mode == s_mode) return 0;

    ESP_LOGI(TAG, "mode switch -> %s",
             mode == ANT_MODE_WIFI ? "WIFI" : "ESPNOW");
    lock();
    s_running = false;   /* pause beacon while restarting */
    unlock();

    stop_transports();

    int rc = 0;
    if (mode == ANT_MODE_WIFI) {
        rc = start_softap();
        if (rc == 0) rc = open_udp_socket();
    } else {
        rc = start_espnow();
    }
    if (rc == 0) {
        ESP_ERROR_CHECK(esp_wifi_set_promiscuous(true));
        ESP_ERROR_CHECK(esp_wifi_set_promiscuous_rx_cb(promiscuous_rx_cb));
        ant_rf_set_tx_power(s_tx_dbm);
        s_mode = mode;
    }
    s_running = true;
    return rc;
}

int ant_rf_set_tx_power(int8_t dbm)
{
    s_tx_dbm = dbm;
    esp_err_t err = esp_wifi_set_max_tx_power(ANT_DBM_TO_IDF(dbm));
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "set_max_tx_power(%d dBm): %s", dbm, esp_err_to_name(err));
        return -1;
    }
    int8_t qdbm = 0;
    if (esp_wifi_get_max_tx_power(&qdbm) == ESP_OK) {
        /* qdbm is in 0.25 dBm units. */
        s_tx_dbm = (int8_t)((qdbm + 2) / 4);  /* round to nearest dBm */
        ESP_LOGI(TAG, "TX power requested %d dBm, actual ~%d dBm", dbm, s_tx_dbm);
    }
    return 0;
}

int8_t ant_rf_get_tx_power_dbm(void) { return s_tx_dbm; }

void ant_rf_on_session_begin(void)
{
    s_logging = true;
    s_link_lost = false;
    s_last_rx_s = esp_timer_get_time() / 1000000;

    const station_session_t *s = session_get();
    ant_mode_t want = ANT_MODE_WIFI;
    if (s) {
        s_session_tx_mob = s->tx_mob;
        s_session_mode   = s->mode;
        want = s->mode;
        /* Apply Station TX immediately. Mode switch is deferred until AFTER
         * PKT_PROTOCOL forward so Mobile (still on SoftAP/UDP) receives
         * tx_mob + commanded mode before both sides leave Mode A. */
        ant_rf_set_tx_power(s->tx_sta);
        ESP_LOGI(TAG, "session RF: mode=%s tx_sta_cmd=%d tx_mob_cmd=%d dBm",
                 want == ANT_MODE_WIFI ? "WIFI" : "ESPNOW",
                 (int)s->tx_sta, (int)s->tx_mob);
    }

    /* Prefer current transport (Quick-Check = WIFI) for the setup frame. */
    ant_rf_forward_protocol();
    /* Give Mobile a beat to reassemble + common mode switch. */
    vTaskDelay(pdMS_TO_TICKS(250));

    if (want != s_mode) {
        ESP_LOGI(TAG, "session: switching Station transport -> %s",
                 want == ANT_MODE_WIFI ? "WIFI" : "ESPNOW");
        ant_rf_set_mode(want);
        /* Re-announce setup on the new transport (best-effort; Mobile may
         * already have switched from the WIFI copy). */
        ant_rf_forward_protocol();
    }
    ESP_LOGI(TAG, "session logging ON");
}

void ant_rf_on_session_end(void)
{
    s_logging = false;
    s_link_lost = false;
    /* Tell Mobile to return to WiFi SoftAP before we leave ESP-NOW so it can
     * rejoin Quick-Check without a menu toggle. */
    s_session_mode = ANT_MODE_WIFI;
    if (s_mode != ANT_MODE_WIFI) {
        ant_rf_forward_protocol();   /* mode=WIFI, no/keeping JSON */
        vTaskDelay(pdMS_TO_TICKS(200));
        ant_rf_set_mode(ANT_MODE_WIFI);
    }
    ESP_LOGI(TAG, "session logging OFF; back to Quick-Check (Mode A WiFi)");
}

void ant_rf_set_protocol_json(const char *json)
{
    lock();
    free(s_protocol_json);
    s_protocol_json = NULL;
    if (json && json[0]) {
        size_t n = strlen(json);
        if (n > ANT_PROTOCOL_JSON_MAX) n = ANT_PROTOCOL_JSON_MAX;
        s_protocol_json = (char *)malloc(n + 1);
        if (s_protocol_json) {
            memcpy(s_protocol_json, json, n);
            s_protocol_json[n] = 0;
        }
    }
    unlock();
}

bool ant_rf_peer_known(void) { return s_peer_known; }
