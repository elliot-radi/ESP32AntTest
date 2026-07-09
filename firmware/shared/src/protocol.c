#include "protocol.h"
#include <string.h>

// ESP32AntTest — packet encode/decode.
// Wire format is little-endian (matches ESP32 native order on both C3 and
// WROOM-32). We serialize byte-by-byte rather than memcpy so the wire layout
// is explicit and not dependent on struct packing/alignment — though the
// struct is __attribute__((packed)) and sizeof is 20 bytes on both targets.

static void put_u16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
}

static void put_u32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF);
    p[3] = (uint8_t)((v >> 24) & 0xFF);
}

static uint16_t get_u16(const uint8_t *p) {
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t get_u32(const uint8_t *p) {
    return (uint32_t)p[0]
        | ((uint32_t)p[1] << 8)
        | ((uint32_t)p[2] << 16)
        | ((uint32_t)p[3] << 24);
}

// Wire layout (20 bytes):
//   0-1   magic (ANT_MAGIC_0, ANT_MAGIC_1)
//   2     version
//   3     type
//   4-7   seq        (LE u32)
//   8-11  session_id (LE u32)
//   12-13 step_id    (LE u16)
//   14    rssi_local (int8)
//   15    tx_power   (int8)
//   16-19 reserved

void ant_packet_encode(const ant_packet_t *pkt, uint8_t *buf) {
    buf[0] = ANT_MAGIC_0;
    buf[1] = ANT_MAGIC_1;
    buf[2] = pkt->version;
    buf[3] = pkt->type;
    put_u32(buf + 4, pkt->seq);
    put_u32(buf + 8, pkt->session_id);
    put_u16(buf + 12, pkt->step_id);
    buf[14] = (uint8_t)pkt->rssi_local;
    buf[15] = (uint8_t)pkt->tx_power;
    memcpy(buf + 16, pkt->reserved, sizeof(pkt->reserved));
}

int ant_packet_decode(const uint8_t *buf, int len, ant_packet_t *out) {
    if (len < (int)sizeof(ant_packet_t)) {
        return -1;
    }
    if (buf[0] != ANT_MAGIC_0 || buf[1] != ANT_MAGIC_1) {
        return -1;
    }
    if (buf[2] != ANT_PROTO_VER) {
        return -1;
    }
    out->magic[0] = buf[0];
    out->magic[1] = buf[1];
    out->version = buf[2];
    out->type = buf[3];
    out->seq = get_u32(buf + 4);
    out->session_id = get_u32(buf + 8);
    out->step_id = get_u16(buf + 12);
    out->rssi_local = (int8_t)buf[14];
    out->tx_power = (int8_t)buf[15];
    memcpy(out->reserved, buf + 16, sizeof(out->reserved));
    return 0;
}
