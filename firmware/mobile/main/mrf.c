/* ESP32AntTest — Mobile RF layer.
 * STA → Station SoftAP, UDP/ESP-NOW beacons, promiscuous RSSI, outage buffer.
 */
#include "mrf.h"
#include "board_config.h"
#include "config.h"
#include "protocol.h"
#include "buffer.h"
#include "ui.h"
#include "button.h"
#include "guide.h"

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
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "esp_now.h"
#include "nvs_flash.h"

static const char *TAG = "mrf";

#define WIFI_CONNECTED_BIT  BIT0
#define WIFI_FAIL_BIT       BIT1

/* Protocol reassembly (matches Station PKT_PROTOCOL extended frame). */
#define PROTO_CHUNK_DATA 200
#define PROTO_JSON_MAX   4096

static EventGroupHandle_t s_wifi_eg;
static SemaphoreHandle_t  s_mu;

static ant_mode_t s_mode = ANT_MODE_WIFI;
static bool       s_running = false;
static bool       s_recording = false;   /* ad-hoc or guided session */
static bool       s_guided    = false;   /* true if host protocol loaded */
static bool       s_step_active = false; /* true after operator pressed ready on current step */
static int8_t     s_tx_dbm = ANT_DEFAULT_TX_POWER;
static int8_t     s_rssi_local = -128;   /* our measurement of Station */
static int8_t     s_rssi_peer  = -128;   /* piggybacked from Station */
static uint32_t   s_seq = 0;
static uint16_t   s_step_id = 0;
static uint32_t   s_session_wire = 0;

static uint8_t    s_peer_mac[6] = {0};
static bool       s_peer_known = false;
static uint32_t   s_sta_ip = 0;          /* network order SoftAP IP */
static int        s_udp_sock = -1;
static bool       s_linked = false;
static int64_t    s_last_rx_us = 0;
static int64_t    s_last_good_piggy_us = 0;  /* last time Station piggyback was valid */

/* AP scan / known SSID prefix */
static char       s_ap_ssid[33] = {0};

/* Protocol reassembly */
static char      *s_proto_buf = NULL;
static uint16_t   s_proto_total = 0;
static uint16_t   s_proto_got = 0;

/* ---- helpers ---- */

static void lock(void)   { if (s_mu) xSemaphoreTake(s_mu, portMAX_DELAY); }
static void unlock(void) { if (s_mu) xSemaphoreGive(s_mu); }

static void update_link_flag(void)
{
    int64_t now = esp_timer_get_time();
    bool was = s_linked;
    s_linked = (s_last_rx_us > 0) &&
               ((now - s_last_rx_us) < (int64_t)ANT_LOSS_THRESHOLD * 1000000);
    if (s_linked != was) ui_set_linked(s_linked);
}

/* Forward one outage buffer entry as PKT_BEACON (RSSI) or PKT_MARKER. */
static void send_raw(const uint8_t *wire, int len);

static void drain_outage_buffer(void)
{
    buf_entry_t e;
    int n = 0;
    while (buffer_pop(&e) && n < 64) {
        ant_packet_t pkt = {0};
        pkt.magic[0] = ANT_MAGIC_0;
        pkt.magic[1] = ANT_MAGIC_1;
        pkt.version  = ANT_PROTO_VER;
        pkt.type     = (e.kind == BUF_KIND_MARKER) ? PKT_MARKER : PKT_BEACON;
        pkt.seq      = e.seq;
        pkt.session_id = s_session_wire;
        pkt.step_id  = e.step_id;
        pkt.rssi_local = e.rssi_mob;  /* Mobile's measurement */
        pkt.tx_power = e.tx_mob;
        /* reserved[0]=ANT_RSV0_MOB_OUTAGE → Station logs source=MOB (rssi_sta empty) */
        if (e.kind == BUF_KIND_RSSI)
            pkt.reserved[0] = ANT_RSV0_MOB_OUTAGE;

        uint8_t wire[sizeof(ant_packet_t)];
        ant_packet_encode(&pkt, wire);
        send_raw(wire, sizeof(wire));
        n++;
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    if (n) ESP_LOGI(TAG, "forwarded %d outage entries", n);
}

/* Buffer a downlink-RSSI sample while Station can't hear our uplink. */
static void buffer_outage_rssi(void)
{
    if (!s_recording) return;
    if (s_rssi_local == (int8_t)-128) return;
    buf_entry_t e = {
        .kind = BUF_KIND_RSSI,
        .seq = s_seq,
        .step_id = s_step_id,
        .rssi_mob = s_rssi_local,
        .rssi_sta = -128,
        .tx_mob = s_tx_dbm,
        .tx_sta = 0,
        .mode = s_mode,
        .timestamp_ms = (uint32_t)(esp_timer_get_time() / 1000),
    };
    buffer_push(&e);
}

static void apply_guide_step_ui(const guide_step_t *g)
{
    if (!g) return;
    s_step_id = g->step_id;
    s_step_active = false;
    ui_set_step(s_step_id);
    ui_set_guide(g->prompt);
    ui_show(UI_SCREEN_GUIDE);
    ESP_LOGI(TAG, "guide step %u/%d: %s",
             (unsigned)g->step_id, guide_step_count(), g->prompt);
}

static void handle_protocol_chunk(const ant_packet_t *hdr,
                                  const uint8_t *ext, int ext_len)
{
    /* Extended: [20 B pkt][u16 total][u16 offset][data...] OR data after hdr
     * only when ext points at +0 relative to full frame past ant_packet. */
    if (ext_len < 4) return;
    uint16_t total  = (uint16_t)(ext[0] | (ext[1] << 8));
    uint16_t offset = (uint16_t)(ext[2] | (ext[3] << 8));
    const uint8_t *data = ext + 4;
    int dlen = ext_len - 4;
    if (total == 0 || total > PROTO_JSON_MAX) return;
    if (offset + dlen > total) dlen = total - offset;

    if (!s_proto_buf || s_proto_total != total) {
        free(s_proto_buf);
        s_proto_buf = (char *)calloc(1, total + 1);
        s_proto_total = total;
        s_proto_got = 0;
    }
    if (!s_proto_buf) return;
    memcpy(s_proto_buf + offset, data, dlen);
    /* Recount approx filled — simple: track high-water. */
    if (offset + dlen > s_proto_got) s_proto_got = offset + dlen;

    if (s_proto_got >= total) {
        s_proto_buf[total] = 0;
        ESP_LOGI(TAG, "protocol received (%u bytes)", total);

        if (guide_load_json(s_proto_buf) == 0 && guide_begin() == 0) {
            s_recording = true;
            s_guided = true;
            s_step_active = false;
            ui_set_recording(true);
            apply_guide_step_ui(guide_current());
        } else {
            ESP_LOGW(TAG, "protocol loaded but no steps — ad-hoc fallback");
            s_recording = true;
            s_guided = false;
            s_step_id = 1;
            s_step_active = true;
            ui_set_recording(true);
            ui_set_step(s_step_id);
            ui_show(UI_SCREEN_SESSION);
        }

        free(s_proto_buf);
        s_proto_buf = NULL;
        s_proto_total = s_proto_got = 0;
    }
}

static void on_peer_packet(const ant_packet_t *pkt, int8_t rssi,
                           const uint8_t *src_mac,
                           const uint8_t *full, int full_len)
{
    lock();
    if (src_mac) {
        memcpy(s_peer_mac, src_mac, 6);
        s_peer_known = true;
    }
    if (rssi != (int8_t)-128) s_rssi_local = rssi;
    if (pkt->type == PKT_BEACON) {
        bool was_linked = s_linked;
        s_rssi_peer = pkt->rssi_local;  /* Station's view of us */
        s_last_rx_us = esp_timer_get_time();
        s_linked = true;
        ui_set_linked(true);
        ui_set_rssi(s_rssi_local, s_rssi_peer);
        ui_bump_samples();
        if (pkt->session_id) s_session_wire = pkt->session_id;

        /* Asymmetric / null-floor capture (ADR-004):
         * If Station's piggyback is valid, the uplink is fine → drain any
         * backlog. If piggyback stays empty for >2 s of continuous downlink
         * (we hear Station, Station is not hearing us), buffer this sample
         * as a MOB outage row to forward later. */
        if (s_rssi_peer != (int8_t)-128) {
            s_last_good_piggy_us = s_last_rx_us;
            if (s_recording && buffer_count() > 0)
                drain_outage_buffer();
        } else if (s_recording && s_rssi_local != (int8_t)-128) {
            int64_t since_good = (s_last_good_piggy_us > 0)
                ? (s_last_rx_us - s_last_good_piggy_us)
                : (s_last_rx_us);  /* never heard a good piggyback this boot */
            /* 2 s grace so cold-start / first beacons don't flood the buffer. */
            if (since_good > 2000000LL)
                buffer_outage_rssi();
            if (!was_linked)
                drain_outage_buffer();  /* WiFi rejoin attempt — try send */
        }
    } else if (pkt->type == PKT_PROTOCOL) {
        const uint8_t *ext = NULL;
        int ext_len = 0;
        if (full && full_len > (int)sizeof(ant_packet_t)) {
            ext = full + sizeof(ant_packet_t);
            ext_len = full_len - (int)sizeof(ant_packet_t);
        }
        handle_protocol_chunk(pkt, ext, ext_len);
    } else if (pkt->type == PKT_MARKER) {
        /* Station shouldn't send markers to us. */
    }
    unlock();
}

/* Promiscuous RSSI cache */
static void IRAM_ATTR promiscuous_rx_cb(void *buf, wifi_promiscuous_pkt_type_t type)
{
    if (type != WIFI_PKT_DATA && type != WIFI_PKT_MGMT) return;
    const wifi_promiscuous_pkt_t *ppkt = (const wifi_promiscuous_pkt_t *)buf;
    if (ppkt->rx_ctrl.sig_len < 16) return;
    const uint8_t *sa = ppkt->payload + 10;
    if (!s_peer_known) return;
    if (memcmp(sa, s_peer_mac, 6) != 0) return;
    s_rssi_local = ppkt->rx_ctrl.rssi;
}

/* ---- send paths ---- */

static void send_raw(const uint8_t *wire, int len)
{
    if (s_mode == ANT_MODE_WIFI) {
        if (s_udp_sock < 0) return;
        struct sockaddr_in dest = {0};
        dest.sin_family = AF_INET;
        dest.sin_port = htons(ANT_UDP_PORT);
        if (s_sta_ip) dest.sin_addr.s_addr = s_sta_ip;
        else dest.sin_addr.s_addr = inet_addr(ANT_STATION_IP);
        sendto(s_udp_sock, wire, len, 0, (struct sockaddr *)&dest, sizeof(dest));
    } else {
        uint8_t bcast[6] = {0xff,0xff,0xff,0xff,0xff,0xff};
        const uint8_t *dst = s_peer_known ? s_peer_mac : bcast;
        esp_now_send(dst, wire, len);
    }
}

static void send_beacon(void)
{
    ant_packet_t pkt = {0};
    pkt.magic[0] = ANT_MAGIC_0;
    pkt.magic[1] = ANT_MAGIC_1;
    pkt.version  = ANT_PROTO_VER;
    pkt.type     = PKT_BEACON;
    pkt.seq      = ++s_seq;
    pkt.session_id = s_session_wire;
    pkt.step_id  = s_step_id;
    pkt.rssi_local = s_rssi_local;
    pkt.tx_power = s_tx_dbm;

    uint8_t wire[sizeof(ant_packet_t)];
    ant_packet_encode(&pkt, wire);
    send_raw(wire, sizeof(wire));

    /* If we are recording and NOT linked, buffer our side for later.
     * (Uplink outage: we may still hear Station — then we don't buffer.) */
    update_link_flag();
}

static void send_marker_now(void)
{
    ant_packet_t pkt = {0};
    pkt.magic[0] = ANT_MAGIC_0;
    pkt.magic[1] = ANT_MAGIC_1;
    pkt.version  = ANT_PROTO_VER;
    pkt.type     = PKT_MARKER;
    pkt.seq      = ++s_seq;
    pkt.session_id = s_session_wire;
    pkt.step_id  = s_step_id;
    pkt.rssi_local = s_rssi_local;
    pkt.tx_power = s_tx_dbm;
    uint8_t wire[sizeof(ant_packet_t)];
    ant_packet_encode(&pkt, wire);

    if (s_linked) {
        send_raw(wire, sizeof(wire));
    } else {
        buf_entry_t e = {
            .kind = BUF_KIND_MARKER,
            .seq = pkt.seq,
            .step_id = s_step_id,
            .rssi_mob = s_rssi_local,
            .rssi_sta = s_rssi_peer,
            .tx_mob = s_tx_dbm,
            .tx_sta = 0,
            .mode = s_mode,
            .timestamp_ms = (uint32_t)(esp_timer_get_time() / 1000),
        };
        buffer_push(&e);
        ESP_LOGI(TAG, "marker buffered (offline) step=%u", s_step_id);
    }
}

/* ---- UDP RX ---- */

static void udp_rx_task(void *arg)
{
    uint8_t buf[512];
    struct sockaddr_in src;
    socklen_t slen = sizeof(src);
    while (1) {
        if (s_udp_sock < 0 || s_mode != ANT_MODE_WIFI) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        int n = recvfrom(s_udp_sock, buf, sizeof(buf), 0,
                         (struct sockaddr *)&src, &slen);
        if (n < (int)sizeof(ant_packet_t)) {
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }
        ant_packet_t pkt;
        if (ant_packet_decode(buf, n, &pkt) != 0) continue;

        /* BSSID / peer: SoftAP MAC from WiFi if known. */
        int8_t rssi = s_rssi_local;
        wifi_ap_record_t ap;
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
            if (rssi == (int8_t)-128) rssi = ap.rssi;
            on_peer_packet(&pkt, rssi, ap.bssid, buf, n);
        } else {
            on_peer_packet(&pkt, rssi, s_peer_known ? s_peer_mac : NULL, buf, n);
        }

        /* Buffer MOB outage samples: if recording and Station does NOT
         * appear to hear us (piggyback rssi empty for a while) we still
         * captured downlink — optional. For v1, Station logs on its RX;
         * MoB buffer is for when WE hear Station during uplink failure,
         * frewarded as better-effort. If linked, no buffer needed. */
        if (s_recording && s_linked == false && pkt.type == PKT_BEACON) {
            /* shouldn't hit if we just set linked */ }
        (void)0;
    }
}

static void espnow_recv_cb(const esp_now_recv_info_t *info,
                           const uint8_t *data, int len)
{
    if (!info || !data || len < (int)sizeof(ant_packet_t)) return;
    ant_packet_t pkt;
    if (ant_packet_decode(data, len, &pkt) != 0) return;
    int8_t rssi = info->rx_ctrl ? info->rx_ctrl->rssi : s_rssi_local;
    if (s_rssi_local != (int8_t)-128) rssi = s_rssi_local;
    on_peer_packet(&pkt, rssi, info->src_addr, data, len);
}

/* ---- WiFi STA ---- */

static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *ev = (wifi_event_sta_disconnected_t *)data;
        s_linked = false;
        ui_set_linked(false);
        xEventGroupClearBits(s_wifi_eg, WIFI_CONNECTED_BIT);
        ESP_LOGW(TAG, "disconnected reason=%d; reconnecting", ev ? ev->reason : -1);
        /* Only reconnect if we have a real SSID target. */
        if (s_ap_ssid[0]) {
            vTaskDelay(pdMS_TO_TICKS(500));
            esp_wifi_connect();
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "got ip: " IPSTR, IP2STR(&e->ip_info.ip));
        /* SoftAP IP = gateway. */
        s_sta_ip = e->ip_info.gw.addr;
        xEventGroupSetBits(s_wifi_eg, WIFI_CONNECTED_BIT);

        wifi_ap_record_t ap;
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
            memcpy(s_peer_mac, ap.bssid, 6);
            s_peer_known = true;
            ESP_LOGI(TAG, "AP BSSID %02X:%02X:%02X:%02X:%02X:%02X rssi=%d",
                     ap.bssid[0], ap.bssid[1], ap.bssid[2],
                     ap.bssid[3], ap.bssid[4], ap.bssid[5], ap.rssi);
            if (s_rssi_local == (int8_t)-128) s_rssi_local = ap.rssi;
        }
        drain_outage_buffer();
    }
}

/* Scan for AntTest-* SoftAP and return its SSID. */
static bool find_station_ssid(char *out, int outsz)
{
    wifi_scan_config_t sc = {
        .ssid = NULL,
        .bssid = NULL,
        .channel = 0,           /* all channels — SoftAP may not be on ch1 yet */
        .show_hidden = true,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
    };
    esp_err_t err = esp_wifi_scan_start(&sc, true);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "scan_start: %s", esp_err_to_name(err));
        return false;
    }
    uint16_t n = 0;
    esp_wifi_scan_get_ap_num(&n);
    if (n == 0) return false;
    wifi_ap_record_t *recs = calloc(n, sizeof(*recs));
    if (!recs) return false;
    uint16_t got = n;
    esp_wifi_scan_get_ap_records(&got, recs);
    bool found = false;
    for (int i = 0; i < got; i++) {
        if (strncmp((char *)recs[i].ssid, ANT_SOFTAP_SSID_PREFIX,
                    strlen(ANT_SOFTAP_SSID_PREFIX)) == 0) {
            snprintf(out, outsz, "%s", (char *)recs[i].ssid);
            found = true;
            ESP_LOGI(TAG, "found AP %s rssi=%d", out, recs[i].rssi);
            break;
        }
    }
    free(recs);
    return found;
}

static int open_udp(void)
{
    if (s_udp_sock >= 0) { close(s_udp_sock); s_udp_sock = -1; }
    int s = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (s < 0) return -1;
    int yes = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    struct timeval tv = { .tv_sec = 0, .tv_usec = 200000 };
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(ANT_UDP_PORT);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(s, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(s);
        return -1;
    }
    s_udp_sock = s;
    return 0;
}

static int start_sta_wifi(void)
{
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    /* Must start WiFi before scan/connect. Empty use-config first. */
    wifi_config_t cfg = {0};
    cfg.sta.threshold.authmode = WIFI_AUTH_OPEN;
    cfg.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    /* Scan for Station SoftAP. Retry a few times. */
    s_ap_ssid[0] = 0;
    for (int attempt = 0; attempt < 8 && !s_ap_ssid[0]; attempt++) {
        if (!find_station_ssid(s_ap_ssid, sizeof(s_ap_ssid))) {
            ESP_LOGW(TAG, "no AntTest AP (try %d)", attempt + 1);
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }
    if (!s_ap_ssid[0]) {
        /* Last resort: prefix alone won't match MAC-suffixed SSID. */
        ESP_LOGW(TAG, "scan failed; will keep scanning in background");
        open_udp();
        return 0;
    }

    strncpy((char *)cfg.sta.ssid, s_ap_ssid, sizeof(cfg.sta.ssid) - 1);
    cfg.sta.ssid[sizeof(cfg.sta.ssid) - 1] = 0;
    cfg.sta.threshold.authmode = WIFI_AUTH_OPEN;
    cfg.sta.channel = ANT_WIFI_CHANNEL;
    cfg.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    /* STA_START handler calls esp_wifi_connect; disconnect any early try. */
    esp_wifi_disconnect();
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &cfg));
    ESP_ERROR_CHECK(esp_wifi_connect());

    EventBits_t bits = xEventGroupWaitBits(s_wifi_eg,
        WIFI_CONNECTED_BIT | WIFI_FAIL_BIT, pdFALSE, pdFALSE,
        pdMS_TO_TICKS(12000));
    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "connected to %s", s_ap_ssid);
    } else {
        ESP_LOGW(TAG, "not yet connected; will keep trying");
    }
    open_udp();
    return 0;
}

static int start_espnow(void)
{
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_channel(ANT_WIFI_CHANNEL, WIFI_SECOND_CHAN_NONE));

    esp_err_t err = esp_now_init();
    if (err != ESP_OK && err != ESP_ERR_ESPNOW_EXIST) {
        ESP_LOGE(TAG, "esp_now_init: %s", esp_err_to_name(err));
        return -1;
    }
    ESP_ERROR_CHECK(esp_now_register_recv_cb(espnow_recv_cb));

    esp_now_peer_info_t peer = {0};
    memset(peer.peer_addr, 0xff, 6);
    peer.channel = ANT_WIFI_CHANNEL;
    peer.ifidx = WIFI_IF_STA;
    peer.encrypt = false;
    esp_now_del_peer(peer.peer_addr);
    esp_now_add_peer(&peer);
    if (s_peer_known) {
        memcpy(peer.peer_addr, s_peer_mac, 6);
        esp_now_del_peer(peer.peer_addr);
        esp_now_add_peer(&peer);
    }
    ESP_LOGI(TAG, "ESP-NOW up");
    return 0;
}

static void beacon_task(void *arg)
{
    const TickType_t period = pdMS_TO_TICKS(1000 / ANT_BEACON_HZ);
    while (1) {
        if (s_running) send_beacon();
        update_link_flag();
        vTaskDelay(period);
    }
}

static void ui_task(void *arg)
{
    const TickType_t period = pdMS_TO_TICKS(1000 / ANT_DISPLAY_HZ);
    while (1) {
        /* button poll ~ every display tick (50ms-ish extra inside) */
        for (int i = 0; i < 5; i++) {
            btn_evt_t ev = button_poll();
            if (ev != BTN_EVT_NONE) {
                if (!ui_handle_button(ev)) {
                    /* unhandled */
                }
            }
            vTaskDelay(pdMS_TO_TICKS(20));
        }
        /* Outage sampling is driven from on_peer_packet (asymmetric uplink). */
        ui_render();
        vTaskDelay(period);
    }
}

/* ---- public ---- */

void ant_mrf_init(void)
{
    s_mu = xSemaphoreCreateMutex();
    s_wifi_eg = xEventGroupCreate();
    buffer_init();

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    } else {
        ESP_ERROR_CHECK(err);
    }
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t wcfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wcfg));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL));

    s_tx_dbm = ANT_DEFAULT_TX_POWER;
    s_mode = ANT_MODE_WIFI;
}

void ant_mrf_start(void)
{
    if (s_running) return;
    start_sta_wifi();
    ESP_ERROR_CHECK(esp_wifi_set_promiscuous(true));
    ESP_ERROR_CHECK(esp_wifi_set_promiscuous_rx_cb(promiscuous_rx_cb));
    ant_mrf_set_tx_power(s_tx_dbm);
    s_running = true;

    xTaskCreate(udp_rx_task, "udp_rx", 8192, NULL, 5, NULL);
    xTaskCreate(beacon_task, "beacon", 8192, NULL, 5, NULL);
    xTaskCreate(ui_task,     "ui",     8192, NULL, 4, NULL);
    ESP_LOGI(TAG, "Mobile RF started (Quick-Check STA)");
}

int ant_mrf_set_mode(ant_mode_t mode)
{
    if (mode == s_mode && s_running) return 0;
    ESP_LOGI(TAG, "mode -> %s", mode == ANT_MODE_WIFI ? "WIFI" : "ESPNOW");
    s_running = false;
    if (s_udp_sock >= 0) { close(s_udp_sock); s_udp_sock = -1; }
    esp_now_deinit();
    esp_wifi_stop();
    s_mode = mode;
    int rc = (mode == ANT_MODE_WIFI) ? start_sta_wifi() : start_espnow();
    if (rc == 0) {
        ESP_ERROR_CHECK(esp_wifi_set_promiscuous(true));
        ESP_ERROR_CHECK(esp_wifi_set_promiscuous_rx_cb(promiscuous_rx_cb));
        ant_mrf_set_tx_power(s_tx_dbm);
    }
    s_running = true;
    ui_set_mode(mode);
    return rc;
}

int ant_mrf_set_tx_power(int8_t dbm)
{
    s_tx_dbm = dbm;
    esp_err_t err = esp_wifi_set_max_tx_power(ANT_DBM_TO_IDF(dbm));
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "tx power: %s", esp_err_to_name(err));
        return -1;
    }
    int8_t q = 0;
    if (esp_wifi_get_max_tx_power(&q) == ESP_OK)
        s_tx_dbm = (int8_t)((q + 2) / 4);
    ui_set_tx(s_tx_dbm);
    return 0;
}

int8_t ant_mrf_get_tx_power_dbm(void) { return s_tx_dbm; }

void ant_mrf_start_adhoc_manual(void)
{
    guide_clear();
    s_recording = true;
    s_guided = false;
    s_step_active = true;
    s_step_id = 1;
    s_session_wire = (uint32_t)(esp_timer_get_time() / 1000000);
    ui_set_recording(true);
    ui_set_step(s_step_id);
    ui_set_guide(NULL);
    ui_show(UI_SCREEN_SESSION);
    ESP_LOGI(TAG, "ad-hoc manual session start step=1");
}

void ant_mrf_end_session(void)
{
    s_recording = false;
    s_guided = false;
    s_step_active = false;
    s_step_id = 0;
    guide_clear();
    ui_set_recording(false);
    ui_set_step(0);
    ui_set_guide(NULL);
    ui_show(UI_SCREEN_QUICKCHECK);
    ESP_LOGI(TAG, "session end");
}

/* Short-press semantics:
 *   guided + not yet "ready" on this step  → mark ready, show live SESSION
 *   guided + ready                        → advance to next step (or done)
 *   ad-hoc                                → bump run counter + PKT_MARKER
 */
bool ant_mrf_on_short_press(void)
{
    if (!s_recording) return false;

    if (s_guided) {
        if (!s_step_active) {
            /* Ready on current step — switch to live RSSI for sampling. */
            s_step_active = true;
            ui_show(UI_SCREEN_SESSION);
            /* Marker announces the step is now the active sample window. */
            send_marker_now();
            ESP_LOGI(TAG, "step %u ready", (unsigned)s_step_id);
            return true;
        }
        /* Advance to next guided step. */
        const guide_step_t *next = guide_advance();
        if (!next) {
            ESP_LOGI(TAG, "protocol complete");
            ui_set_guide("DONE - long=end");
            ui_show(UI_SCREEN_GUIDE);
            s_step_active = false;
            /* Leave recording on until operator ends; Station keeps logging. */
            return true;
        }
        apply_guide_step_ui(next);
        return true;
    }

    /* Ad-hoc: bump run counter + marker. */
    s_step_id++;
    ui_set_step(s_step_id);
    send_marker_now();
    ESP_LOGI(TAG, "ad-hoc advance -> step %u", s_step_id);
    return true;
}

void ant_mrf_advance_step(void)
{
    (void)ant_mrf_on_short_press();
}

bool ant_mrf_is_linked(void) { return s_linked; }
bool ant_mrf_is_recording(void) { return s_recording; }
bool ant_mrf_is_guided(void) { return s_guided; }
uint16_t ant_mrf_step_id(void) { return s_step_id; }
int ant_mrf_step_index(void) { return guide_cursor(); }
int ant_mrf_step_count(void) { return guide_step_count(); }
