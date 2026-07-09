#pragma once
#include <stdint.h>

// ESP32AntTest — shared packet protocol
// See docs/SPEC.md §5 and ADR-004 (beacon sampling model).
//
// Both boards transmit PKT_BEACON autonomously; each beacon piggybacks the
// sender's freshest RSSI measurement of the peer in rssi_local, so a single
// received beacon yields both directional RSSI values.

#define ANT_MAGIC_0     0xAE
#define ANT_MAGIC_1     0x32
#define ANT_PROTO_VER   0x01

typedef enum {
    PKT_BEACON    = 0x01,  // autonomous broadcast; carries piggybacked rssi_local
    PKT_MARKER    = 0x04,  // button-press marker (forwarded from Mobile during/after outage)
    PKT_PROTOCOL  = 0x10,  // session-setup: chunked protocol transfer (once per session, out-of-band)
    PKT_MODE_ACK  = 0x11,
} ant_pkt_type_t;

typedef enum {
    ANT_MODE_WIFI   = 0x01,
    ANT_MODE_ESPNOW = 0x02,
} ant_mode_t;

typedef struct __attribute__((packed)) {
    uint8_t  magic[2];      // ANT_MAGIC_0, ANT_MAGIC_1
    uint8_t  version;       // ANT_PROTO_VER
    uint8_t  type;          // ant_pkt_type_t
    uint32_t seq;
    uint32_t session_id;
    uint16_t step_id;      // active protocol step (run); ad-hoc: incrementing run counter
    int8_t   rssi_local;    // RSSI measured locally by sender (of peer's last beacon) — piggyback
    int8_t   tx_power;      // sender's current TX power in dBm
    uint8_t  reserved[4];
} ant_packet_t;             // 20 bytes (LE wire format; see protocol.c)

// Validate and decode a received buffer into ant_packet_t
// Returns 0 on success, -1 on magic/version mismatch
int ant_packet_decode(const uint8_t *buf, int len, ant_packet_t *out);

// Encode ant_packet_t into a byte buffer (buf must be >= sizeof(ant_packet_t))
void ant_packet_encode(const ant_packet_t *pkt, uint8_t *buf);
