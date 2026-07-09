/* Unit tests for the shared packet encode/decode.
 *
 * Pure host C (no ESP-IDF) — the protocol code is portable, so we compile it
 * with the system compiler and run on the host. Build with the Makefile:
 *   make test
 */
#include "protocol.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int failures = 0;

#define CHECK(cond) \
    do { \
        if (!(cond)) { \
            failures++; \
            printf("FAIL: %s (line %d)\n", #cond, __LINE__); \
        } \
    } while (0)

#define CHECK_EQ_INT(a, b) \
    do { \
        if ((a) != (b)) { \
            failures++; \
            printf("FAIL: %s == %s: got %lld, want %lld (line %d)\n", \
                   #a, #b, (long long)(a), (long long)(b), __LINE__); \
        } \
    } while (0)

static void fill_pkt(ant_packet_t *p, uint32_t seq, uint32_t sid,
                     uint16_t step, int8_t rssi, int8_t tx, uint8_t type) {
    memset(p, 0, sizeof(*p));
    p->magic[0] = ANT_MAGIC_0;
    p->magic[1] = ANT_MAGIC_1;
    p->version = ANT_PROTO_VER;
    p->type = type;
    p->seq = seq;
    p->session_id = sid;
    p->step_id = step;
    p->rssi_local = rssi;
    p->tx_power = tx;
    p->reserved[0] = 0xDE;
    p->reserved[1] = 0xAD;
    p->reserved[2] = 0xBE;
    p->reserved[3] = 0xEF;
}

int main(void) {
    /* Sanity: the documented wire size. (reserved[4] makes this 20, not 16.) */
    CHECK_EQ_INT((int)sizeof(ant_packet_t), 20);

    /* ---- Round-trip: encode then decode preserves all fields ---- */
    {
        ant_packet_t in, out;
        uint8_t buf[sizeof(ant_packet_t) + 4]; /* extra tail to prove decode ignores it */
        fill_pkt(&in, 0x12345678u, 0xCAFEBABEu, 0x0123, -67, 17, PKT_BEACON);
        ant_packet_encode(&in, buf);
        /* decode with a few extra trailing bytes (must be ignored) */
        memset(buf + sizeof(ant_packet_t), 0xFF, 4);
        CHECK_EQ_INT(ant_packet_decode(buf, (int)sizeof(buf), &out), 0);

        CHECK_EQ_INT(out.magic[0], ANT_MAGIC_0);
        CHECK_EQ_INT(out.magic[1], ANT_MAGIC_1);
        CHECK_EQ_INT(out.version, ANT_PROTO_VER);
        CHECK_EQ_INT(out.type, PKT_BEACON);
        CHECK_EQ_INT((unsigned)out.seq, 0x12345678u);
        CHECK_EQ_INT((unsigned)out.session_id, 0xCAFEBABEu);
        CHECK_EQ_INT(out.step_id, 0x0123);
        CHECK_EQ_INT(out.rssi_local, -67);
        CHECK_EQ_INT(out.tx_power, 17);
        CHECK(out.reserved[0] == 0xDE && out.reserved[1] == 0xAD &&
              out.reserved[2] == 0xBE && out.reserved[3] == 0xEF);
    }

    /* ---- Edge values: zero, max, and signed extremes round-trip ---- */
    {
        ant_packet_t in, out;
        uint8_t buf[sizeof(ant_packet_t)];
        fill_pkt(&in, 0u, 0xFFFFFFFFu, 0u, -128, 127, PKT_MARKER);
        ant_packet_encode(&in, buf);
        CHECK_EQ_INT(ant_packet_decode(buf, (int)sizeof(buf), &out), 0);
        CHECK_EQ_INT((unsigned)out.seq, 0u);
        CHECK_EQ_INT((unsigned)out.session_id, 0xFFFFFFFFu);
        CHECK_EQ_INT(out.step_id, 0u);
        CHECK_EQ_INT(out.rssi_local, -128);
        CHECK_EQ_INT(out.tx_power, 127);
        CHECK_EQ_INT(out.type, PKT_MARKER);
    }

    /* ---- Little-endian byte order of seq at offset 4 ---- */
    {
        ant_packet_t in;
        uint8_t buf[sizeof(ant_packet_t)];
        memset(&in, 0, sizeof(in));
        in.magic[0] = ANT_MAGIC_0; in.magic[1] = ANT_MAGIC_1;
        in.version = ANT_PROTO_VER; in.type = PKT_BEACON;
        in.seq = 0x11223344u;
        ant_packet_encode(&in, buf);
        CHECK_EQ_INT(buf[4], 0x44);
        CHECK_EQ_INT(buf[5], 0x33);
        CHECK_EQ_INT(buf[6], 0x22);
        CHECK_EQ_INT(buf[7], 0x11);
    }

    /* ---- Rejection: bad magic (byte 0) ---- */
    {
        ant_packet_t in, out;
        uint8_t buf[sizeof(ant_packet_t)];
        fill_pkt(&in, 1, 2, 3, -50, 10, PKT_BEACON);
        ant_packet_encode(&in, buf);
        buf[0] = 0x00;
        CHECK_EQ_INT(ant_packet_decode(buf, (int)sizeof(buf), &out), -1);
    }

    /* ---- Rejection: bad magic (byte 1) ---- */
    {
        ant_packet_t in, out;
        uint8_t buf[sizeof(ant_packet_t)];
        fill_pkt(&in, 1, 2, 3, -50, 10, PKT_BEACON);
        ant_packet_encode(&in, buf);
        buf[1] = 0x00;
        CHECK_EQ_INT(ant_packet_decode(buf, (int)sizeof(buf), &out), -1);
    }

    /* ---- Rejection: wrong version ---- */
    {
        ant_packet_t in, out;
        uint8_t buf[sizeof(ant_packet_t)];
        fill_pkt(&in, 1, 2, 3, -50, 10, PKT_BEACON);
        ant_packet_encode(&in, buf);
        buf[2] = ANT_PROTO_VER + 1;
        CHECK_EQ_INT(ant_packet_decode(buf, (int)sizeof(buf), &out), -1);
    }

    /* ---- Rejection: buffer too short ---- */
    {
        ant_packet_t in, out;
        uint8_t buf[sizeof(ant_packet_t)];
        fill_pkt(&in, 1, 2, 3, -50, 10, PKT_BEACON);
        ant_packet_encode(&in, buf);
        CHECK_EQ_INT(ant_packet_decode(buf, (int)sizeof(ant_packet_t) - 1, &out), -1);
    }

    /* ---- Accept: exactly-sized buffer (len == sizeof) ---- */
    {
        ant_packet_t in, out;
        uint8_t buf[sizeof(ant_packet_t)];
        fill_pkt(&in, 7, 8, 9, -71, 20, PKT_PROTOCOL);
        ant_packet_encode(&in, buf);
        CHECK_EQ_INT(ant_packet_decode(buf, (int)sizeof(ant_packet_t), &out), 0);
        CHECK_EQ_INT(out.type, PKT_PROTOCOL);
        CHECK_EQ_INT(out.seq, 7u);
    }

    if (failures == 0) {
        printf("PASS: all protocol encode/decode tests passed\n");
        return 0;
    }
    printf("FAIL: %d failure(s)\n", failures);
    return 1;
}
